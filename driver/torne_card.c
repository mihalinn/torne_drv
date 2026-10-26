// SPDX-License-Identifier: GPL-2.0-only
/*
 * torne_card.c - the card slot of the Sony CECH-ZD1 J (a smart-card UART in the MB86H47A) with ISO 7816-3 T=1
 *
 * Port of coax's mb86h47/card.go and pkg/iso7816 (Till0196, Apache-2.0 OR GPL-2.0-or-later).
 * The slot is driven register by register through the bridge; this file speaks T=1 over it and gives APDU level
 * Reset/Transmit calls (what a PC/SC reader driver offers).
 */
#include "torne_card.h"

/* UART registers of the card slot (MB86H47A, 0xFEA000) */
#define CARD_ENABLE 0xfea000
#define CARD_LINES 0xfea004 /* bit 1 clock, bit 2 reset released, bit 3 VCC */
#define CARD_BAUD 0xfea008
#define CARD_SEND 0xfea00c /* 1 sends what the transmit FIFO holds */
#define CARD_CONTROL 0xfea010 /* speed setting, then the transmit command word */
#define CARD_FIFO 0xfea014 /* bits 16-31: bytes waiting to be read */
#define CARD_IRQ_MASK 0xfea018
#define CARD_IRQ 0xfea01c /* bit 10: no card in the slot */
#define CARD_CWT 0xfea020
#define CARD_BWT 0xfea024
#define CARD_TX 0xfea028
#define CARD_RX 0xfea02c /* a read takes the next byte */
#define CARD_POWER 0xfea034
#define CARD_SENT_LRC 0xfea038
#define CARD_RECV_LRC 0xfea03c
#define CARD_NO_CARD (1u << 10)

/* line settings per speed: baud, CWT, BWT, control */
struct card_speed {
	int rate;
	u32 baud, cwt, bwt, control;
};

static const struct card_speed card_speeds[] = {
	{ 9600, 0xba7f, 9600, 0, 0 },
	{ 19200, 0x5d5d, 107, 36559, 1 << 16 },
	{ 38400, 0x2eae, 107, 0xffff, 1 << 16 },
};

/* block types */
#define BLOCK_INFORMATION 0x00
#define BLOCK_MORE_DATA 0x20
#define BLOCK_RECEIVE_READY 0x80
#define BLOCK_SUPERVISORY 0xc0
#define BLOCK_RESPONSE 0x20
#define BLOCK_RESYNCH 0x00
#define BLOCK_IFS 0x01
#define BLOCK_WTX 0x03

#define NODE_ADDRESS 0x00
#define MAX_FRAME 255
#define MAX_INFO_FIELD 254
#define PROLOGUE_LENGTH 3
#define MAX_BLOCK (PROLOGUE_LENGTH + MAX_INFO_FIELD + 2)
#define MAX_ATR 33
#define DEFAULT_IFSC 32

#define POLL_INTERVAL_MS 5
#define ATR_TIMEOUT_MS 1000
#define DEFAULT_BLOCK_WAIT_MS 500
#define MAX_BLOCK_WAIT_MS 3000
#define MIN_BLOCK_WAIT_MS 500
#define OPERATION_TIMEOUT_MS 3000
#define MAX_RETRIES 3
#define MAX_DISCARD_ROUNDS 4

static inline u64 now(struct torne_card *c)
{
	return c->br->ops->now_ms(c->br->priv);
}

static inline void sleep_ms(struct torne_card *c, unsigned int ms)
{
	c->br->ops->msleep(c->br->priv, ms);
}

/* ---- the UART ---- */

static u32 card_read(struct torne_card *c, u32 address)
{
	return torne_mem_read32(c->br, address);
}

static void card_write(struct torne_card *c, u32 address, u32 value)
{
	torne_mem_write32(c->br, address, value);
}

static void card_set(struct torne_card *c, u32 address, u32 bits)
{
	card_write(c, address, card_read(c, address) | bits);
}

static void card_clear(struct torne_card *c, u32 address, u32 bits)
{
	card_write(c, address, card_read(c, address) & ~bits);
}

/* runs a line operation with the bridge's error state cleared; returns the first error */
#define LINE_BEGIN(c) ((c)->br->err = 0)
#define LINE_END(c) ((c)->br->err)

static int line_present(struct torne_card *c, bool *present)
{
	u32 irq;

	LINE_BEGIN(c);
	irq = card_read(c, CARD_IRQ);
	*present = !(irq & CARD_NO_CARD);
	return LINE_END(c);
}

static void power_down_card(struct torne_card *c)
{
	card_clear(c, CARD_LINES, 4);
	card_write(c, CARD_FIFO, 0);
	card_clear(c, CARD_ENABLE, 4);
	card_clear(c, CARD_LINES, 0xa);
	card_write(c, CARD_IRQ_MASK, 1);
	card_write(c, CARD_POWER, 0);
}

/* powers the card down and up again, leaving it about to send its answer to reset at 9600 baud */
static int line_reset(struct torne_card *c)
{
	LINE_BEGIN(c);
	power_down_card(c);
	sleep_ms(c, 10);
	card_write(c, CARD_IRQ, 0x3ff);
	card_write(c, CARD_POWER, 1);
	card_clear(c, CARD_LINES, 4);
	card_write(c, CARD_FIFO, 0);
	card_write(c, CARD_ENABLE, 0);
	card_write(c, CARD_ENABLE, 7);
	card_set(c, CARD_LINES, 8); /* VCC */
	card_write(c, CARD_BAUD, card_speeds[0].baud);
	sleep_ms(c, 2);
	card_set(c, CARD_LINES, 2); /* clock */
	card_write(c, CARD_RECV_LRC, 0x3b);
	card_write(c, CARD_CWT, card_speeds[0].cwt);
	card_write(c, CARD_BWT, 0);
	card_write(c, CARD_IRQ_MASK, 0x1ff);
	sleep_ms(c, 1);
	card_set(c, CARD_LINES, 4); /* reset released */
	return LINE_END(c);
}

static int line_set_baudrate(struct torne_card *c, int rate)
{
	unsigned int i;

	for (i = 0; i < sizeof(card_speeds) / sizeof(card_speeds[0]); i++) {
		if (card_speeds[i].rate != rate)
			continue;
		LINE_BEGIN(c);
		card_write(c, CARD_BAUD, card_speeds[i].baud);
		card_write(c, CARD_CONTROL, card_speeds[i].control);
		card_write(c, CARD_CWT, card_speeds[i].cwt);
		card_write(c, CARD_BWT, card_speeds[i].bwt);
		return LINE_END(c);
	}
	return -EINVAL;
}

static int line_ready(struct torne_card *c, bool *ready)
{
	u32 fifo;

	LINE_BEGIN(c);
	fifo = card_read(c, CARD_FIFO);
	*ready = (fifo >> 16) != 0;
	return LINE_END(c);
}

/* takes up to len of the bytes that have arrived; returns the count or a negative error */
static int line_read(struct torne_card *c, u8 *buf, unsigned int len)
{
	unsigned int waiting, n, i;

	LINE_BEGIN(c);
	waiting = card_read(c, CARD_FIFO) >> 16;
	if (c->br->err || waiting == 0)
		return c->br->err;
	n = waiting < len ? waiting : len;
	for (i = 0; i < n && !c->br->err; i++)
		buf[i] = (u8)card_read(c, CARD_RX);
	if (c->br->err)
		return c->br->err;
	return n;
}

/* sends a block, check byte included */
static int line_write(struct torne_card *c, const u8 *data, unsigned int len)
{
	unsigned int i;
	u32 command = 0x200;

	LINE_BEGIN(c);
	card_write(c, CARD_RECV_LRC, 0);
	card_write(c, CARD_SENT_LRC, 0);
	for (i = 0; i < len && !c->br->err; i++)
		card_write(c, CARD_TX, data[i]);
	card_write(c, CARD_IRQ, 0x3ff);
	/* the transmit command word, written before every send; frames of 256 bytes and more take 0x280 */
	if (len >= 0x100)
		command = 0x280;
	card_write(c, CARD_CONTROL, command);
	card_write(c, CARD_SEND, 1);
	return LINE_END(c);
}

/* ---- checks ---- */

static u8 lrc(const u8 *data, unsigned int len)
{
	u8 result = 0;
	unsigned int i;

	for (i = 0; i < len; i++)
		result ^= data[i];
	return result;
}

/* CRC-16/CCITT-FALSE, the two-byte check some cards ask for */
static u16 crc16(const u8 *data, unsigned int len)
{
	u16 crc = 0xffff;
	unsigned int i;
	int bit;

	for (i = 0; i < len; i++) {
		crc ^= (u16)data[i] << 8;
		for (bit = 0; bit < 8; bit++) {
			if (crc & 0x8000)
				crc = (u16)(crc << 1) ^ 0x1021;
			else
				crc <<= 1;
		}
	}
	return crc;
}

static inline int edc_length(struct torne_card *c)
{
	return c->use_crc ? 2 : 1;
}

/* ---- ATR ---- */

struct atr_parameters {
	int baudrate;
	int ifsc;
	unsigned int block_wait_ms;
	bool use_crc;
};

static unsigned int block_wait_for(u8 index)
{
	unsigned int wait = MAX_BLOCK_WAIT_MS;

	if (index < 5)
		wait = 100u << index;
	if (wait < MIN_BLOCK_WAIT_MS)
		wait = MIN_BLOCK_WAIT_MS;
	if (wait > MAX_BLOCK_WAIT_MS)
		wait = MAX_BLOCK_WAIT_MS;
	return wait;
}

/* -EAGAIN while the answer is still a valid prefix, -EBADMSG once it cannot become one */
static int parse_atr(const u8 *atr, unsigned int len, struct atr_parameters *p)
{
	unsigned int offset = 2, historical, expected;
	int group = 1;
	bool t1_group = false, has_protocol_byte = false;
	u8 present;

	p->baudrate = 9600;
	p->ifsc = DEFAULT_IFSC;
	p->block_wait_ms = DEFAULT_BLOCK_WAIT_MS;
	p->use_crc = false;
	if (len < 2)
		return -EAGAIN;
	if (atr[0] != 0x3b && atr[0] != 0x3f)
		return -EBADMSG;

	present = atr[1] >> 4;
	historical = atr[1] & 0x0f;

	for (;;) {
		if (present & 0x01) {
			u8 ta;

			if (offset >= len)
				return -EAGAIN;
			ta = atr[offset++];
			if (group == 1) {
				switch (ta) {
				case 0x11:
					p->baudrate = 9600;
					break;
				case 0x12:
					p->baudrate = 19200;
					break;
				case 0x13:
					p->baudrate = 38400;
					break;
				default:
					return -EOPNOTSUPP;
				}
			} else if (t1_group && group >= 3) {
				if (ta >= 1 && ta <= 254)
					p->ifsc = ta;
			}
		}
		if (present & 0x02) {
			u8 tb;

			if (offset >= len)
				return -EAGAIN;
			tb = atr[offset++];
			if (t1_group && group >= 3)
				p->block_wait_ms = block_wait_for(tb >> 4);
		}
		if (present & 0x04) {
			u8 tc;

			if (offset >= len)
				return -EAGAIN;
			tc = atr[offset++];
			if (t1_group && group >= 3)
				p->use_crc = tc & 0x01;
		}
		if (!(present & 0x08))
			break;
		{
			u8 td, protocol;

			if (offset >= len)
				return -EAGAIN;
			td = atr[offset++];
			protocol = td & 0x0f;
			/* a card offering anything but T=0 closes its answer with a check byte */
			if (protocol != 0)
				has_protocol_byte = true;
			t1_group = protocol == 1;
			present = td >> 4;
			group++;
		}
	}

	expected = offset + historical;
	if (has_protocol_byte)
		expected++;
	if (expected > MAX_ATR)
		return -EBADMSG;
	if (len < expected)
		return -EAGAIN;
	if (len > expected)
		return -EBADMSG;
	if (has_protocol_byte && lrc(atr + 1, len - 1) != 0)
		return -EBADMSG;
	return 0;
}

static int read_atr(struct torne_card *c, struct atr_parameters *p)
{
	u64 deadline = now(c) + ATR_TIMEOUT_MS;
	unsigned int len = 0;

	for (;;) {
		bool ready;
		int n, rc;
		u8 chunk[MAX_ATR];

		if (len) {
			rc = parse_atr(c->atr, len, p);
			if (rc == 0) {
				c->atr_len = len;
				return 0;
			}
			if (rc != -EAGAIN)
				return rc;
		}
		if (now(c) > deadline)
			return -ETIMEDOUT;
		rc = line_ready(c, &ready);
		if (rc)
			return rc;
		if (!ready) {
			sleep_ms(c, POLL_INTERVAL_MS);
			continue;
		}
		n = line_read(c, chunk, sizeof(chunk));
		if (n < 0)
			return n;
		if (n == 0) {
			sleep_ms(c, POLL_INTERVAL_MS);
			continue;
		}
		if (len + n > MAX_ATR)
			return -EBADMSG;
		memcpy(c->atr + len, chunk, n);
		len += n;
	}
}

/* ---- T=1 blocks ---- */

static int send1(struct torne_card *c, u8 pcb, const u8 *data, unsigned int dlen)
{
	u8 frame[MAX_FRAME];
	unsigned int length = PROLOGUE_LENGTH + dlen + edc_length(c);

	if (length > MAX_FRAME)
		return -EMSGSIZE;
	frame[0] = NODE_ADDRESS;
	frame[1] = pcb;
	frame[2] = dlen;
	if (dlen)
		memcpy(frame + PROLOGUE_LENGTH, data, dlen);
	if (c->use_crc) {
		u16 check = crc16(frame, PROLOGUE_LENGTH + dlen);

		frame[PROLOGUE_LENGTH + dlen] = check >> 8;
		frame[PROLOGUE_LENGTH + dlen + 1] = check;
	} else {
		frame[PROLOGUE_LENGTH + dlen] = lrc(frame, PROLOGUE_LENGTH + dlen);
	}
	return line_write(c, frame, length);
}

static int verify(struct torne_card *c, const u8 *frame, unsigned int len)
{
	if (c->use_crc) {
		u16 want = crc16(frame, len - 2);
		u16 got = (u16)frame[len - 2] << 8 | frame[len - 1];

		return want == got ? 0 : -EBADMSG;
	}
	return lrc(frame, len) == 0 ? 0 : -EBADMSG;
}

/* reads one block, waiting for it until the deadline; only as much as the block still needs is read */
static int receive1(struct torne_card *c, u64 deadline, u8 *pcb, u8 *data, unsigned int *dlen)
{
	u8 frame[MAX_BLOCK];
	unsigned int len = 0;
	int rc;

	for (;;) {
		unsigned int want = PROLOGUE_LENGTH - len;
		bool ready;
		int n;

		if (len >= PROLOGUE_LENGTH) {
			unsigned int expected = PROLOGUE_LENGTH + frame[2] + edc_length(c);

			if (len >= expected)
				break;
			want = expected - len;
		}
		if (now(c) > deadline)
			return -ETIMEDOUT;
		rc = line_ready(c, &ready);
		if (rc)
			return rc;
		if (!ready) {
			sleep_ms(c, POLL_INTERVAL_MS);
			continue;
		}
		n = line_read(c, frame + len, want);
		if (n < 0)
			return n;
		if (n == 0) {
			sleep_ms(c, POLL_INTERVAL_MS);
			continue;
		}
		len += n;
	}

	if (frame[0] != NODE_ADDRESS)
		return -EBADMSG;
	rc = verify(c, frame, len);
	if (rc)
		return rc;
	*pcb = frame[1];
	*dlen = frame[2];
	memcpy(data, frame + PROLOGUE_LENGTH, frame[2]);
	return 0;
}

/* clears whatever is left on the line, so that a broken exchange does not leave a prologue for the next one */
static int discard(struct torne_card *c)
{
	int round;

	for (round = 0; round < MAX_DISCARD_ROUNDS; round++) {
		bool ready;
		u8 chunk[MAX_FRAME];
		int rc = line_ready(c, &ready), n;

		if (rc)
			return rc;
		if (!ready)
			return 0;
		n = line_read(c, chunk, sizeof(chunk));
		if (n < 0)
			return n;
	}
	return -EBADMSG;
}

static bool supervisory_request(u8 pcb, u8 kind)
{
	return (pcb & 0xc0) == BLOCK_SUPERVISORY && !(pcb & BLOCK_RESPONSE) && (pcb & 0x1f) == kind;
}

static bool is_retryable(int err)
{
	return err == -ETIMEDOUT || err == -EBADMSG;
}

/* sends one block and returns the card's, retrying the pair on the failures a retransmission can fix and
 * answering the card's requests for more time */
static int exchange(struct torne_card *c, u8 pcb, const u8 *data, unsigned int dlen, int retries, u64 deadline, u8 *rpcb,
		    u8 *rdata, unsigned int *rlen)
{
	int last_err = 0, attempt;

	for (attempt = 0; attempt < retries; attempt++) {
		unsigned int wait;
		int rc;

		/* sending again after the deadline has gone cannot help */
		if (now(c) > deadline)
			break;
		if (attempt != 0) {
			/* whatever the failed attempt left on the line would otherwise be read as the front of the answer */
			rc = discard(c);
			if (rc)
				return rc;
		}
		rc = send1(c, pcb, data, dlen);
		if (rc)
			return rc;
		/* wait grows when the card asks for more time, and is always cut short by the operation's own deadline */
		wait = c->block_wait_ms;
		for (;;) {
			u64 block_deadline = now(c) + wait;
			u8 answered, answer[MAX_INFO_FIELD];
			unsigned int answer_len;

			if (block_deadline > deadline)
				block_deadline = deadline;
			rc = receive1(c, block_deadline, &answered, answer, &answer_len);
			if (rc) {
				last_err = rc;
				break;
			}
			if (supervisory_request(answered, BLOCK_WTX)) {
				/* the card is asking for more time, as a multiple of the wait it is otherwise allowed */
				if (answer_len != 1 || answer[0] == 0)
					return -EBADMSG;
				wait = c->block_wait_ms * answer[0];
				rc = send1(c, BLOCK_SUPERVISORY | BLOCK_RESPONSE | BLOCK_WTX, answer, 1);
				if (rc)
					return rc;
				continue;
			}
			if (supervisory_request(answered, BLOCK_IFS)) {
				/* a card may set the block size in its own direction too, at any point */
				if (answer_len != 1)
					return -EBADMSG;
				rc = send1(c, BLOCK_SUPERVISORY | BLOCK_RESPONSE | BLOCK_IFS, answer, 1);
				if (rc)
					return rc;
				continue;
			}
			if ((answered & 0xc0) == BLOCK_RECEIVE_READY) {
				/* the card is asking for a block back: if it wants the one we just sent, it did not take it */
				if ((pcb & 0x80) == BLOCK_INFORMATION && ((answered >> 4) & 1) == ((pcb >> 6) & 1)) {
					last_err = -EBADMSG;
					break;
				}
			}
			*rpcb = answered;
			*rlen = answer_len;
			memcpy(rdata, answer, answer_len);
			return 0;
		}
		/* only a lost or corrupted block is worth sending again */
		if (!is_retryable(last_err))
			return last_err;
	}
	return last_err ? last_err : -ETIMEDOUT;
}

static void invalidate(struct torne_card *c)
{
	c->open = false;
	c->atr_len = 0;
	c->send = 0;
	c->receive = 0;
	c->use_crc = false;
	c->ifsc = DEFAULT_IFSC;
	c->block_wait_ms = DEFAULT_BLOCK_WAIT_MS;
	c->baudrate = 9600;
}

void torne_card_setup(struct torne_card *c, struct torne_bridge *br)
{
	memset(c, 0, sizeof(*c));
	c->br = br;
	invalidate(c);
}

void torne_card_invalidate(struct torne_card *c)
{
	invalidate(c);
}

int torne_card_present(struct torne_card *c, bool *present)
{
	return line_present(c, present);
}

/* puts both ends back to block number zero */
static int resynchronise(struct torne_card *c, u64 deadline)
{
	u8 pcb, data[MAX_INFO_FIELD];
	unsigned int len;
	int rc = exchange(c, BLOCK_SUPERVISORY | BLOCK_RESYNCH, NULL, 0, 1, deadline, &pcb, data, &len);

	if (rc)
		return rc;
	if (pcb != (BLOCK_SUPERVISORY | BLOCK_RESPONSE | BLOCK_RESYNCH) || len != 0)
		return -EBADMSG;
	c->send = 0;
	c->receive = 0;
	return 0;
}

/* tells the card how much this host will accept and checks it agrees */
static int agree_ifs(struct torne_card *c, int ifsd, int retries, u64 deadline)
{
	u8 pcb, data[MAX_INFO_FIELD], req = (u8)ifsd;
	unsigned int len;
	int rc = exchange(c, BLOCK_SUPERVISORY | BLOCK_IFS, &req, 1, retries, deadline, &pcb, data, &len);

	if (rc)
		return rc;
	if (pcb != (BLOCK_SUPERVISORY | BLOCK_RESPONSE | BLOCK_IFS) || len != 1 || data[0] != ifsd)
		return -EBADMSG;
	return 0;
}

static int our_ifsd(struct torne_card *c)
{
	if (c->baudrate == 38400)
		return 254;
	if (c->use_crc)
		return MAX_FRAME - PROLOGUE_LENGTH - 2;
	return MAX_FRAME - PROLOGUE_LENGTH - 1;
}

/* agrees the block size with the card. A card on one of the two slower speeds is resynchronised first and then asked
 * for a block size, while one on the fastest speed is asked straight away -- resynchronisation is what upsets it --
 * and resynchronised only if that ask fails. */
static int initialize(struct torne_card *c)
{
	u64 deadline = now(c) + OPERATION_TIMEOUT_MS;
	int ifsd = our_ifsd(c), rc;

	if (c->baudrate != 38400) {
		rc = resynchronise(c, deadline);
		if (rc)
			return rc;
		return agree_ifs(c, ifsd, MAX_RETRIES, deadline);
	}
	rc = agree_ifs(c, ifsd, 1, deadline);
	if (rc == 0)
		return 0;
	if (rc != -EBADMSG && rc != -ETIMEDOUT)
		return rc;
	rc = resynchronise(c, deadline);
	if (rc)
		return rc;
	return agree_ifs(c, ifsd, 1, deadline);
}

int torne_card_reset(struct torne_card *c, u8 *atr, unsigned int *atr_len)
{
	struct atr_parameters parameters;
	bool present;
	int rc, attempt;

	invalidate(c);

	rc = line_present(c, &present);
	if (rc)
		return rc;
	if (!present)
		return -ENOMEDIUM;

	/* two attempts, and only for one reason: bytes left over from a previous conversation can precede the answer on a
	 * back-to-back reset. Any other failure is real. */
	for (attempt = 0; attempt < 2; attempt++) {
		rc = line_reset(c);
		if (rc)
			return rc;
		rc = read_atr(c, &parameters);
		if (rc == 0)
			break;
		if (attempt != 0 || rc != -EBADMSG)
			return rc;
	}
	if (rc)
		return rc;

	rc = line_set_baudrate(c, parameters.baudrate);
	if (rc)
		return rc;
	c->baudrate = parameters.baudrate;
	c->use_crc = parameters.use_crc;
	c->ifsc = parameters.ifsc;
	c->block_wait_ms = parameters.block_wait_ms;

	rc = initialize(c);
	if (rc) {
		invalidate(c);
		return rc;
	}
	c->open = true;
	memcpy(atr, c->atr, c->atr_len);
	*atr_len = c->atr_len;
	return 0;
}

/* sends one command APDU and returns the card's response, including its status bytes */
int torne_card_transmit(struct torne_card *c, const u8 *apdu, unsigned int apdu_len, u8 *resp, unsigned int *resp_len)
{
	u64 deadline;
	u8 pcb;
	const u8 *block = apdu;
	unsigned int block_len = apdu_len, total = 0, limit, part;
	bool present;
	int rc;

	if (apdu_len == 0)
		return -EINVAL;
	if (!c->open)
		return -ENOTCONN;
	limit = (unsigned int)c->ifsc;
	if (limit > MAX_INFO_FIELD)
		limit = MAX_INFO_FIELD;
	if (limit > (unsigned int)(MAX_FRAME - PROLOGUE_LENGTH - edc_length(c)))
		limit = MAX_FRAME - PROLOGUE_LENGTH - edc_length(c);
	if (apdu_len > limit)
		return -EMSGSIZE;

	rc = line_present(c, &present);
	if (rc) {
		invalidate(c);
		return rc;
	}
	if (!present) {
		invalidate(c);
		return -ENOMEDIUM;
	}

	deadline = now(c) + OPERATION_TIMEOUT_MS;
	pcb = BLOCK_INFORMATION | (c->send << 6);

	/* a card that never clears its more-data bit would chain for ever, so the run is bounded */
	for (part = 0; part <= MAX_FRAME; part++) {
		u8 received, data[MAX_INFO_FIELD];
		unsigned int len;

		rc = exchange(c, pcb, block, block_len, MAX_RETRIES, deadline, &received, data, &len);
		if (rc)
			goto fail;
		if ((received & 0x80) == BLOCK_INFORMATION) {
			/* the card only sends an information block once it has taken ours */
			if (part == 0)
				c->send ^= 1;
			if (((received >> 6) & 1) != c->receive) {
				rc = -EBADMSG;
				goto fail;
			}
			c->receive ^= 1;
			if (total + len > *resp_len) {
				rc = -EOVERFLOW;
				goto fail;
			}
			memcpy(resp + total, data, len);
			total += len;
			if (!(received & BLOCK_MORE_DATA)) {
				*resp_len = total;
				return 0;
			}
			/* more to come: acknowledge with a receive-ready block asking for the next */
			pcb = BLOCK_RECEIVE_READY | (c->receive << 4);
			block = NULL;
			block_len = 0;
		} else {
			rc = -EBADMSG;
			goto fail;
		}
	}
	rc = -EBADMSG;
fail:
	/* anything that went wrong mid-conversation leaves the block numbers out of step, so the whole session goes */
	invalidate(c);
	return rc;
}
