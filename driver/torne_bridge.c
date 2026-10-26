// SPDX-License-Identifier: GPL-2.0-only
/*
 * torne_bridge.c - Fujitsu MB86H47A USB bridge of the Sony CECH-ZD1 J
 *
 * Port of coax's mb86h47 package (Till0196, Apache-2.0 OR GPL-2.0-or-later).
 *
 * The host reaches every block of the chip through vendor request 0xbc, which
 * reads and writes chip memory (big-endian words). Everything else is built on
 * that: the memory request handler is widened to 32-bit addresses, two codec
 * cores (FR30 CPUs) are started with small programs (I2C master, stream
 * packetizer), and the TS engine feeds the demodulator's parallel output into
 * an SDRAM ring that core 0 sends to bulk endpoint 0x81 in 192-byte units.
 *
 * Not ported (performance only): the register ring and the bulk command
 * channel of codec core 1. Every I2C access goes through the mailbox.
 */
#include "torne_core.h"
#include "torne_regs.h"

struct torne_handler_word {
	u32 address, original, widened;
};
#include "torne_fw_gen.h"

#define REQUEST_MEMORY 0xbc
#define VENDOR_OUT 0x40
#define VENDOR_IN 0xc0

#define READ_LIMIT 64
#define WRITE_LIMIT 64

#define ADDRESS_STATE 0x82008
#define REG_GPIO_DIRECTION 0x90014
#define REG_GPIO_VALUE 0x90018

static const u32 addressing_banks[2] = { 0xe1000, 0xe1800 };
#define ADDRESSING_BANK_SIZE 0x800
#define ADDRESSING_WORDS_N 5

/* where the (original) memory request handler is patched, see the Go sources */
#define VECTOR_TABLE 0xffc00
#define FIRST_FREE_VECTOR 20
#define TRAP_STUBS 0xe3000
#define TRAP_COMMON 0xe3800
#define INTERRUPT_LEVELS 0x440

#define CORE1_BASE 0x1700000
#define CORE1_ORIGIN 0xa0000
#define CORE0_BASE 0x1600000
#define CORE0_ORIGIN 0xa0000
#define MAILBOX 0xa3f00
#define MAILBOX_WRITE 0x04
#define MAILBOX_ALIVE 0x7c
#define MAILBOX_ANSWER 0x80
#define RING_SCRATCH 0xa3d00
#define RING_ADDRESSES 0xa3d40
#define RING 0xa3e00
#define RING_SLOTS 16
#define RING_ANSWER 0xa3e40
#define RING_FAILURE 0xa3e44
#define I2C_MAX_TRANSFER 32
#define I2C_TIMEOUT_MS 500

/* core 1's command channel variables, which a fresh core 1 expects to be set */
#define COMMAND_VARS 0xa3c00
#define COMMAND_EXPECT 0x00
#define COMMAND_SLOT 0x04
#define COMMAND_ERROR 0x14
#define COMMAND_BASE 0x82100000u

#define RING_BASE 0x00ae4000
#define RING_SIZE 0x00600000
#define ENGINE_BASE 0xd00000

enum { I2C_OK = 0, I2C_BUS_ERROR, I2C_NO_ADDRESS_ACK, I2C_NO_DATA_ACK, I2C_STUCK };

#define LOG(b, lvl, ...)                                                   \
	do {                                                               \
		if ((b)->ops->log && (lvl) <= (b)->log_level)              \
			(b)->ops->log((b)->priv, (lvl), __VA_ARGS__);      \
	} while (0)

static inline u32 be32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static inline void put_be32(u8 *p, u32 v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

void torne_bridge_init(struct torne_bridge *b, const struct torne_ops *ops, void *priv)
{
	memset(b, 0, sizeof(*b));
	b->ops = ops;
	b->priv = priv;
	b->log_level = TORNE_LOG_ERR;
}

/* ---- memory accesses ---- */

/* wValue carries address bits 16-23 in its high byte and, once the request is
 * widened, bits 24-31 in its low byte; wIndex the low 16 bits. */
static void memory_setup(u32 address, u16 *value, u16 *index)
{
	*value = (u16)((address >> 8) & 0xff00) | (u16)(address >> 24);
	*index = (u16)address;
}

static void keep_err(struct torne_bridge *b, int err)
{
	if (err && !b->err)
		b->err = err;
}

static u32 read32(struct torne_bridge *b, u32 address)
{
	u8 buf[4];
	u16 value, index;
	int n;

	memory_setup(address, &value, &index);
	n = b->ops->control(b->priv, VENDOR_IN, value, index, buf, 4);
	if (n != 4) {
		if (n != -ENODEV)
			LOG(b, TORNE_LOG_ERR, "MB86H47A read %#08x: %d", address, n);
		keep_err(b, n < 0 ? n : -EIO);
		return 0;
	}
	return be32(buf);
}

static void write32(struct torne_bridge *b, u32 address, u32 data)
{
	u8 buf[4];
	u16 value, index;
	int n;

	memory_setup(address, &value, &index);
	put_be32(buf, data);
	n = b->ops->control(b->priv, VENDOR_OUT, value, index, buf, 4);
	if (n != 4) {
		if (n != -ENODEV)
			LOG(b, TORNE_LOG_ERR, "MB86H47A write %#08x: %d", address, n);
		keep_err(b, n < 0 ? n : -EIO);
	}
}

static void read_block(struct torne_bridge *b, u32 address, u8 *buf, unsigned int len)
{
	u16 value, index;
	int n;

	memory_setup(address, &value, &index);
	n = b->ops->control(b->priv, VENDOR_IN, value, index, buf, len);
	if (n != (int)len) {
		if (n != -ENODEV)
			LOG(b, TORNE_LOG_ERR, "MB86H47A read %#08x: %d bytes", address, n);
		keep_err(b, n < 0 ? n : -EIO);
	}
}

static void write_block(struct torne_bridge *b, u32 address, const u8 *buf, unsigned int len)
{
	u16 value, index;
	int n;

	memory_setup(address, &value, &index);
	n = b->ops->control(b->priv, VENDOR_OUT, value, index, (void *)buf, len);
	if (n != (int)len) {
		if (n != -ENODEV)
			LOG(b, TORNE_LOG_ERR, "MB86H47A write %#08x: %d", address, n);
		keep_err(b, n < 0 ? n : -EIO);
	}
}

u32 torne_mem_read32(struct torne_bridge *b, u32 address)
{
	return read32(b, address);
}

void torne_mem_write32(struct torne_bridge *b, u32 address, u32 value)
{
	write32(b, address, value);
}

/* 16-bit registers sit in the high half of their word */
static void write16(struct torne_bridge *b, u32 address, u16 data)
{
	u32 old = read32(b, address);

	write32(b, address, (u32)data << 16 | (old & 0xffff));
}

static u16 read16(struct torne_bridge *b, u32 address)
{
	return read32(b, address) >> 16;
}

static void set32(struct torne_bridge *b, u32 address, u32 bits)
{
	write32(b, address, read32(b, address) | bits);
}

static void clear32(struct torne_bridge *b, u32 address, u32 bits)
{
	write32(b, address, read32(b, address) & ~bits);
}

static void write_words(struct torne_bridge *b, u32 address, const u32 *words, unsigned int count)
{
	u8 buf[WRITE_LIMIT];
	unsigned int off, n, i;

	for (off = 0; off < count && !b->err; off += n) {
		n = count - off;
		if (n > WRITE_LIMIT / 4)
			n = WRITE_LIMIT / 4;
		for (i = 0; i < n; i++)
			put_be32(buf + i * 4, words[off + i]);
		write_block(b, address + off * 4, buf, n * 4);
	}
}

/* ---- widening of the memory request ---- */

static bool jump_target(u32 word, u32 *target)
{
	if ((word >> 16 & 0xff0f) != 0x9b0c)
		return false;
	*target = (word >> 20 & 0xf) << 16 | (word & 0xffff);
	return true;
}

static int addressing_bank(u32 address)
{
	int bank;

	for (bank = 0; bank < 2; bank++)
		if (address >= addressing_banks[bank] && address < addressing_banks[bank] + ADDRESSING_BANK_SIZE)
			return bank;
	return -1;
}

static const u32 *addressing_code(int bank)
{
	return bank == 0 ? torne_addressing0_code : torne_addressing1_code;
}

static const struct torne_handler_word *addressing_words(int bank)
{
	return bank == 0 ? torne_addressing0_words : torne_addressing1_words;
}

static int load_addressing(struct torne_bridge *b, int bank)
{
	const u32 *code = addressing_code(bank);
	u32 base = addressing_banks[bank];
	unsigned int i;

	for (i = 0; i < TORNE_ADDRESSING_WORDS; i++)
		write32(b, base + i * 4, code[i]);
	for (i = 0; i < TORNE_ADDRESSING_WORDS; i++) {
		u32 got = read32(b, base + i * 4);

		if (got != code[i] && !b->err) {
			LOG(b, TORNE_LOG_ERR, "MB86H47A SRAM at %#x reads %08x, want %08x", base + i * 4, got, code[i]);
			return -EIO;
		}
	}
	return b->err;
}

static int widen(struct torne_bridge *b)
{
	const struct torne_handler_word *fresh = torne_addressing0_words;
	u32 held[ADDRESSING_WORDS_N];
	int bank = -1;
	unsigned int jumps = 0, widened = 0, i;

	for (i = 0; i < ADDRESSING_WORDS_N; i++) {
		u32 target;
		bool is_jump;

		held[i] = read32(b, fresh[i].address);
		if (b->err)
			return b->err;
		is_jump = jump_target(held[i], &target);
		if (held[i] == fresh[i].original) {
			/* untouched */
		} else if (held[i] == fresh[i].widened && !is_jump) {
			widened++;
		} else if (is_jump && addressing_bank(target) >= 0 && (bank < 0 || bank == addressing_bank(target))) {
			bank = addressing_bank(target);
			jumps++;
		} else {
			LOG(b, TORNE_LOG_ERR, "MB86H47A memory request handler at %#x is not the known one", fresh[i].address);
			return -EIO;
		}
	}
	if (jumps + widened < ADDRESSING_WORDS_N) {
		/* not widened, or only partly: the jumps do not go anywhere yet, bank 0 is free */
		int err = load_addressing(b, 0);

		if (err)
			return err;
		for (i = 0; i < ADDRESSING_WORDS_N; i++)
			write32(b, fresh[i].address, fresh[i].widened);
		return b->err;
	}
	{
		const struct torne_handler_word *words = addressing_words(bank);
		const u32 *code = addressing_code(bank);
		bool same = true;
		u8 loaded[TORNE_ADDRESSING_WORDS * 4];
		unsigned int off;

		for (i = 0; i < ADDRESSING_WORDS_N; i++)
			same = same && held[i] == words[i].widened;
		if (same) {
			for (off = 0; off < sizeof(loaded); off += READ_LIMIT) {
				unsigned int n = sizeof(loaded) - off;

				if (n > READ_LIMIT)
					n = READ_LIMIT;
				read_block(b, addressing_banks[bank] + off, loaded + off, n);
			}
			for (i = 0; i < TORNE_ADDRESSING_WORDS; i++)
				same = same && be32(loaded + i * 4) == code[i];
			if (b->err)
				return b->err;
			if (same)
				return 0;
		}
		bank = 1 - bank;
		words = addressing_words(bank);
		{
			int err = load_addressing(b, bank);

			if (err)
				return err;
		}
		for (i = 0; i < ADDRESSING_WORDS_N; i++)
			if (held[i] != words[i].widened)
				write32(b, words[i].address, words[i].widened);
		return b->err;
	}
}

/* ---- clocks, interrupts, codec core loading ---- */

static const u32 standby_blocks[TORNE_STANDBY_BLOCKS] = {
	TORNE_STANDBY_BLOCK_0_MODE, TORNE_STANDBY_BLOCK_1_MODE, TORNE_STANDBY_BLOCK_2_MODE,
	TORNE_STANDBY_BLOCK_3_MODE, TORNE_STANDBY_BLOCK_4_MODE, TORNE_STANDBY_BLOCK_5_MODE,
	TORNE_STANDBY_BLOCK_6_MODE, TORNE_STANDBY_BLOCK_7_MODE, TORNE_STANDBY_BLOCK_8_MODE,
};

static void clocks(struct torne_bridge *b)
{
	unsigned int i;
	static const u32 chroma[4] = { TORNE_REG_MEMORY_P2_2_C0, TORNE_REG_MEMORY_P2_2_C1,
				       TORNE_REG_MEMORY_P2_4_C0, TORNE_REG_MEMORY_P2_4_C1 };

	write16(b, TORNE_REG_CLOCK_PLL, 0x8201);
	write16(b, TORNE_REG_CLOCK_UNK114, 0x8008);
	write16(b, TORNE_REG_CLOCK_GATES, 0xffff);
	/* bit 2 of each engine block's standby register (16 bits, in the high half of the word) */
	for (i = 0; i < TORNE_STANDBY_BLOCKS; i++)
		set32(b, standby_blocks[i], TORNE_CLUSTER_STANDBY_WAIT << 16);
	write16(b, TORNE_REG_CLOCK_UNK11C, 6);
	write16(b, TORNE_REG_CLOCK_UNK128, 0x6000);
	write16(b, TORNE_REG_CLOCK_UNK12C, 0x4007);
	write16(b, TORNE_REG_MEMORY_FORMAT2_1, 3);
	for (i = 0; i < 4; i++)
		write16(b, chroma[i], 0x0880);
}

static void trap_interrupts(struct torne_bridge *b)
{
	unsigned int i, vector;
	u32 offset;

	/* ICR00-ICR63: level 31 masks a source */
	for (offset = 0; offset < 0x40; offset += 4) {
		u32 word = read32(b, INTERRUPT_LEVELS + offset);

		if ((word | 0x1f1f1f1f) != word)
			write32(b, INTERRUPT_LEVELS + offset, word | 0x1f1f1f1f);
	}
	for (i = 0; i < TORNE_TRAP_RECORDER_WORDS; i++)
		write32(b, TRAP_COMMON + i * 4, torne_trap_recorder[i]);
	write_words(b, TRAP_STUBS, torne_trap_stubs, TORNE_TRAP_STUBS_WORDS);
	for (vector = 1; vector < 256; vector++) {
		if (vector >= 16 && vector < FIRST_FREE_VECTOR)
			continue;
		write32(b, VECTOR_TABLE + 0x3fc - 4 * vector, TRAP_STUBS + vector * TORNE_TRAP_STUB_STRIDE);
	}
}

/*
 * Holds the codec core at base in reset and loads the program at origin. The
 * program goes sixteen words to a request and only where it differs from what
 * is read back.
 */
static void load_core(struct torne_bridge *b, u32 base, u32 origin, const u32 *program, unsigned int words)
{
	u8 loaded[64];
	unsigned int first, n, i;

	write32(b, base + 0xffffc, origin);
	write32(b, base, 0xffffffff);
	for (first = 0; first < words && !b->err; first += n) {
		n = words - first;
		if (n > sizeof(loaded) / 4)
			n = sizeof(loaded) / 4;
		read_block(b, base + origin + first * 4, loaded, n * 4);
		for (i = 0; i < n; i++) {
			if (be32(loaded + i * 4) != program[first + i]) {
				write_words(b, base + origin + first * 4, program + first, n);
				break;
			}
		}
	}
}

static void i2c_init(struct torne_bridge *b)
{
	set32(b, TORNE_REG_USB_ENABLE, TORNE_USB_BLOCK_I2C);
	clear32(b, TORNE_REG_USB_RESET, TORNE_USB_BLOCK_I2C);
	write32(b, TORNE_REG_USB_UNK0C0, 1);
	write32(b, TORNE_REG_USB_UNK0C0, 0);
	write32(b, TORNE_REG_USB_UNK0C4, 0);
	write16(b, 0x90114, 0x8008);
	write32(b, TORNE_REG_I2C_UNK18, 0x06);
	/* the bus clock's divider: 0x10 is about 100 kHz, 0x08 doubles it */
	write32(b, TORNE_REG_I2C_DIVIDER, 0x08);
	write32(b, TORNE_REG_I2C_CLOCK, 0x3c);
	write32(b, TORNE_REG_I2C_CONTROL, 0x40);
}

/* starts codec core 1 (the I2C transfers) and waits to see it run */
static int core1_start(struct torne_bridge *b, u32 seed)
{
	u32 before, after;
	u8 zero[RING_SLOTS * 4];
	unsigned int i;
	static const u32 clear_at[] = { MAILBOX, MAILBOX + MAILBOX_ANSWER, MAILBOX + MAILBOX_ALIVE,
					RING_SCRATCH, RING_ADDRESSES, RING_FAILURE };

	load_core(b, CORE1_BASE, CORE1_ORIGIN, torne_core1_program, TORNE_CORE1_PROGRAM_WORDS);
	for (i = 0; i < sizeof(clear_at) / sizeof(clear_at[0]); i++)
		write32(b, CORE1_BASE + clear_at[i], 0);
	memset(zero, 0, sizeof(zero));
	write_block(b, CORE1_BASE + RING, zero, sizeof(zero));
	/* the command channel starts from a new sequence number at the first slot */
	write32(b, CORE1_BASE + COMMAND_VARS + COMMAND_SLOT, COMMAND_BASE);
	write32(b, CORE1_BASE + COMMAND_VARS + COMMAND_ERROR, 0);
	write32(b, CORE1_BASE + COMMAND_VARS + COMMAND_EXPECT, seed);
	/* the ring starts at slot 16, whose lap is 1; the answer names the slot before */
	write32(b, CORE1_BASE + RING_ANSWER, (u32)(RING_SLOTS - 1) << 24);
	write32(b, CORE1_BASE, 0xfffcffff);
	b->sequence = 0;
	before = read32(b, CORE1_BASE + MAILBOX + MAILBOX_ALIVE);
	b->ops->msleep(b->priv, 1);
	after = read32(b, CORE1_BASE + MAILBOX + MAILBOX_ALIVE);
	if (b->err)
		return b->err;
	if (before == after) {
		LOG(b, TORNE_LOG_ERR, "MB86H47A codec core 1 did not start");
		return -EIO;
	}
	return 0;
}

int torne_bridge_start(struct torne_bridge *b)
{
	u16 state;
	int err;

	b->err = 0;
	state = read16(b, ADDRESS_STATE);
	if (b->err)
		return b->err;
	if (state != 0) {
		LOG(b, TORNE_LOG_ERR,
		    "MB86H47A is not in its power-up state; unplug the CECH-ZD1 J and plug it back in");
		return -EBUSY;
	}
	err = widen(b);
	if (err)
		return err;
	clocks(b);
	trap_interrupts(b);
	i2c_init(b);
	if (b->err)
		return b->err;
	return core1_start(b, ((u32)b->ops->now_ms(b->priv) & 0x7fffffff) | 1);
}

int torne_bridge_reset_demodulator(struct torne_bridge *b)
{
	u16 value, direction;

	b->err = 0;
	value = read16(b, REG_GPIO_VALUE);
	direction = read16(b, REG_GPIO_DIRECTION);
	write16(b, REG_GPIO_VALUE, (value & ~1) | 1 << 6);
	write16(b, REG_GPIO_DIRECTION, direction | 1 | 1 << 6);
	b->ops->msleep(b->priv, 50);
	write16(b, REG_GPIO_VALUE, value | 1 | 1 << 6);
	b->ops->msleep(b->priv, 10);
	return b->err;
}

/* ---- I2C master through core 1's mailbox ---- */

static int i2c_error(struct torne_bridge *b, u8 address, u8 code)
{
	switch (code) {
	case I2C_OK:
		return 0;
	case I2C_BUS_ERROR:
		LOG(b, TORNE_LOG_ERR, "MB86H47A I2C bus error at %#02x", address);
		return -EIO;
	case I2C_NO_ADDRESS_ACK:
		LOG(b, TORNE_LOG_ERR, "MB86H47A I2C address %#02x not acknowledged", address);
		return -ENXIO;
	case I2C_NO_DATA_ACK:
		LOG(b, TORNE_LOG_ERR, "MB86H47A I2C data to %#02x not acknowledged", address);
		return -EIO;
	default:
		LOG(b, TORNE_LOG_ERR, "MB86H47A I2C timeout at %#02x", address);
		return -ETIMEDOUT;
	}
}

/* one transfer through the mailbox: write (if any), then read (if any) after a repeated start */
static int i2c_transfer(struct torne_bridge *b, u8 address, const u8 *wdata, unsigned int wlen, u8 *rdata,
			unsigned int rlen)
{
	u8 answer[4 + I2C_MAX_TRANSFER];
	unsigned int off, answer_len = 4 + ((rlen + 3) & ~3u);
	u64 deadline;
	int err;

	if (wlen > I2C_MAX_TRANSFER || rlen > I2C_MAX_TRANSFER) {
		LOG(b, TORNE_LOG_ERR, "MB86H47A I2C transfer of %u+%u bytes is longer than %d", wlen, rlen,
		    I2C_MAX_TRANSFER);
		return -EINVAL;
	}
	b->err = 0;
	if (++b->sequence == 0)
		b->sequence = 1;
	for (off = 0; off < wlen; off += 4) {
		u8 word[4] = { 0, 0, 0, 0 };
		unsigned int n = wlen - off < 4 ? wlen - off : 4;

		memcpy(word, wdata + off, n);
		write32(b, CORE1_BASE + MAILBOX + MAILBOX_WRITE + off, be32(word));
	}
	write32(b, CORE1_BASE + MAILBOX, (u32)b->sequence << 24 | (u32)address << 16 | wlen << 8 | rlen);
	deadline = b->ops->now_ms(b->priv) + I2C_TIMEOUT_MS;
	for (;;) {
		read_block(b, CORE1_BASE + MAILBOX + MAILBOX_ANSWER, answer, answer_len);
		if (b->err)
			return b->err;
		if (answer[0] == b->sequence)
			break;
		if (b->ops->now_ms(b->priv) > deadline) {
			LOG(b, TORNE_LOG_ERR, "MB86H47A I2C transfer to %#02x: no answer from codec core 1", address);
			return -ETIMEDOUT;
		}
	}
	err = i2c_error(b, address, answer[3]);
	if (err)
		return err;
	if (rlen)
		memcpy(rdata, answer + 4, rlen);
	return 0;
}

int torne_i2c_write(struct torne_bridge *b, u8 address, const u8 *data, unsigned int len)
{
	return i2c_transfer(b, address, data, len, NULL, 0);
}

int torne_i2c_read(struct torne_bridge *b, u8 address, u8 *data, unsigned int len)
{
	return i2c_transfer(b, address, NULL, 0, data, len);
}

int torne_i2c_write_read(struct torne_bridge *b, u8 address, const u8 *wdata, unsigned int wlen, u8 *rdata,
			 unsigned int rlen)
{
	return i2c_transfer(b, address, wdata, wlen, rdata, rlen);
}

/* ---- stream ---- */

/* the stream setup (the TS engine's input and writer, core 0, endpoint 0x81's DMA) */
static void stream_setup(struct torne_bridge *b)
{
	static const u32 zero_a[] = { 0xd01040, 0xd01064, 0xd010a0, 0xd010a4, 0xd010a8 };
	static const u32 zero_b[] = { 0xd02028, 0xd0202c, 0xd02030, 0xd02038 };
	unsigned int i;

	write32(b, 0xd000c4, 0); /* no interrupts to the main CPU */
	write32(b, 0xd00000, 0xffffffff);
	/* pulse the sub-block resets while keeping the engine's core (bits 16-17) in reset */
	write32(b, 0xd00004, 0x001f33ff);
	write32(b, 0xd00000, 0x30030fff);
	write32(b, 0xd00000, 0x00030000);
	write32(b, 0xd00004, 0x00003305);
	for (i = 0; i < sizeof(zero_a) / sizeof(zero_a[0]); i++)
		write32(b, zero_a[i], 0);
	write32(b, 0xd01020, 0x02000000); /* hold the input */
	write32(b, 0xd01004, 0);
	write32(b, 0xd01044, 0x00bc0000); /* 188-byte packets */
	write32(b, 0xd01048, 0x47000000); /* sync byte */
	write32(b, 0xd0104c, 0x00ffffff);
	write32(b, 0xd01050, 0);
	write32(b, 0xd01060, 0);
	write32(b, 0xd01000, 0x06070000); /* port mode */
	write32(b, 0xd01080, 0x01000000); /* timestamp: 192-byte units */
	write32(b, 0xd01088, 0);
	write32(b, 0xd0108c, 0xff);
	write32(b, 0xd01084, 0);
	write32(b, 0xd01090, 0x01000000);
	for (i = 0; i < sizeof(zero_b) / sizeof(zero_b[0]); i++)
		write32(b, zero_b[i], 0);
	write32(b, 0xd02000, 0x03020300);
	write32(b, 0xd02020, RING_BASE);
	write32(b, 0xd02024, RING_SIZE);
	write32(b, 0xd00004, 0x00043305);

	if (!b->core0_loaded) {
		load_core(b, CORE0_BASE, CORE0_ORIGIN, torne_core0_program, TORNE_CORE0_PROGRAM_WORDS);
		b->core0_loaded = b->err == 0;
	} else {
		write32(b, CORE0_BASE, 0xffffffff);
	}
	write32(b, CORE0_BASE, 0xfffcffff);

	set32(b, TORNE_REG_USB_ENABLE, TORNE_USB_BLOCK_STREAM);
	write32(b, TORNE_REG_USB_DMA0_CONTROL, 1);
	write32(b, TORNE_REG_USB_DMA1_CONTROL, 1);
	clear32(b, TORNE_REG_USB_RESET, TORNE_USB_BLOCK_STREAM);
	write32(b, TORNE_REG_USB_DMA2_CONTROL, 0);
	write32(b, TORNE_REG_USB_DMA5_CONTROL, 0);
	write32(b, TORNE_REG_USB_DMA0_CONTROL, 0);
	write32(b, TORNE_REG_USB_DMA0_CONFIG, 0xb09f0000);
	write32(b, TORNE_REG_USB_DMA0_CONTROL, 2);
	write32(b, TORNE_REG_USB_DMA1_CONTROL, 0);
	write32(b, TORNE_REG_USB_DMA1_CONFIG, 0xb09f2000);
	write32(b, TORNE_REG_USB_DMA1_CONTROL, 2);
	/* channel DMA[8] would take what arrives on endpoint 0x02; it stays held */
	write32(b, TORNE_REG_USB_DMA8_CONTROL, 1);
	write32(b, TORNE_REG_USB_DMA8_CONFIG, 0x00008004);
	/* USB-core registers take whole words */
	write32(b, TORNE_REG_USB_EP1_CONTROL, 0x00ff6001); /* EP1 reset */
	for (i = 0; i < 100 && !b->err && (read32(b, TORNE_REG_USB_EP1_CONTROL) & 1); i++)
		;
	write32(b, TORNE_REG_USB_EP1_CONTROL, 0x00ff6000);
	write32(b, TORNE_REG_USB_EPMODE, 0x00060030); /* bit 16+n: EPn in DMA mode; bit n: EPn interrupt */
}

int torne_bridge_prepare_stream(struct torne_bridge *b)
{
	b->err = 0;
	stream_setup(b);
	return b->err;
}

/* lets the demodulator's output in: the input, its timestamp clock and the ring writer */
int torne_bridge_start_stream(struct torne_bridge *b)
{
	b->err = 0;
	write32(b, 0xd01020, 0);
	write32(b, 0xd01040, 0x03000000); /* input on */
	write32(b, 0xd010a4, 0x13170400);
	write32(b, 0xd00484, 0x01000000); /* timestamp clock */
	write32(b, 0xd00488, 0x206c0000);
	write32(b, 0xd02010, 0x02000000); /* writer on */
	if (b->err)
		return b->err;
	b->streaming = true;
	return 0;
}

/* holds core 0 and the engine's input, so nothing more reaches the endpoint */
int torne_bridge_stop_stream(struct torne_bridge *b)
{
	b->streaming = false;
	b->err = 0;
	write32(b, CORE0_BASE, 0xffffffff);
	write32(b, 0xd01040, 0);
	write32(b, 0xd01020, 0x02000000);
	write32(b, 0xd02010, 0x04000000);
	return b->err;
}

/* ---- framer: 192-byte units (4-byte arrival timestamp + TS packet) to TS packets ---- */

#define TIMESTAMP_SIZE (TORNE_SOURCE_PACKET_SIZE - TORNE_TS_PACKET_SIZE)
#define SYNC_BYTE 0x47

void torne_framer_reset(struct torne_framer *f)
{
	f->len = 0;
}

/* the next offset from which a unit starts: a sync byte where the TS packet begins and, when the data
 * reaches that far, another one a unit later; a lone 0x47 in the payload is too common to trust */
static size_t resync(const u8 *data, size_t len, size_t from)
{
	size_t offset, next, last;

	for (offset = from; offset + TORNE_SOURCE_PACKET_SIZE <= len; offset++) {
		if (data[offset + TIMESTAMP_SIZE] != SYNC_BYTE)
			continue;
		next = offset + TORNE_SOURCE_PACKET_SIZE + TIMESTAMP_SIZE;
		if (next >= len || data[next] == SYNC_BYTE)
			return offset;
	}
	last = len + 1 > TORNE_SOURCE_PACKET_SIZE ? len - TORNE_SOURCE_PACKET_SIZE + 1 : 0;
	return from > last ? from : last;
}

/* takes whole units off data, appends their TS packets to out and returns the offset of the first byte
 * not used (less than a unit remains from there) */
size_t torne_ts_extract(const u8 *data, size_t len, u8 *out, size_t *written)
{
	size_t offset = 0, w = 0;

	while (offset + TORNE_SOURCE_PACKET_SIZE <= len) {
		const u8 *packet = data + offset + TIMESTAMP_SIZE;

		if (packet[0] != SYNC_BYTE) {
			offset = resync(data, len, offset + 1);
			continue;
		}
		memcpy(out + w, packet, TORNE_TS_PACKET_SIZE);
		w += TORNE_TS_PACKET_SIZE;
		offset += TORNE_SOURCE_PACKET_SIZE;
	}
	*written = w;
	return offset;
}

size_t torne_framer_feed(struct torne_framer *f, const u8 *raw, size_t raw_len, u8 *out)
{
	size_t total = 0;

	while (raw_len) {
		size_t room = TORNE_FRAMER_CAP - f->len;
		size_t take = raw_len < room ? raw_len : room;
		size_t written, used;

		memcpy(f->buf + f->len, raw, take);
		f->len += take;
		raw += take;
		raw_len -= take;
		used = torne_ts_extract(f->buf, f->len, out + total, &written);
		total += written;
		f->len -= used;
		if (f->len)
			memmove(f->buf, f->buf + used, f->len);
	}
	return total;
}