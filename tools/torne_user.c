// SPDX-License-Identifier: GPL-2.0-only
/*
 * torne_user.c - user-space test harness for the torne driver core (libusb)
 *
 *   torne_user emu-trace            run the bridge against a fake memory device and print the request trace
 *   torne_user probe                Start the bridge, reset the demodulator, read the DiB8000 identity
 *   torne_user tune CH [SECONDS]    tune a terrestrial channel and write the TS to stdout
 */
#include <libusb-1.0/libusb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "torne_core.h"
#include "torne_card.h"
#ifdef WITH_DIB
#include "torne_dib.h"
#endif

/* ---- common ---- */

static u64 mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void do_msleep(void *priv, unsigned int ms)
{
	(void)priv;
	usleep(ms * 1000);
}

static u64 do_now_ms(void *priv)
{
	(void)priv;
	return mono_ms();
}

static void do_log(void *priv, int level, const char *fmt, ...)
{
	va_list ap;

	(void)priv;
	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ---- libusb transport ---- */

struct usb_ctx {
	libusb_context *ctx;
	libusb_device_handle *h;
	unsigned long requests;
};

static int usb_control(void *priv, u8 request_type, u16 value, u16 index, void *buf, u16 len)
{
	struct usb_ctx *u = priv;

	u->requests++;
	return libusb_control_transfer(u->h, request_type, 0xbc, value, index, buf, len, 1000);
}

static int usb_bulk(void *priv, u8 endpoint, void *buf, int len, unsigned int timeout_ms)
{
	struct usb_ctx *u = priv;
	int got = 0, r;

	r = libusb_bulk_transfer(u->h, endpoint, buf, len, &got, timeout_ms);
	if (r == LIBUSB_ERROR_TIMEOUT)
		return got > 0 ? got : -ETIMEDOUT;
	return r < 0 ? -EIO : got;
}

static const struct torne_ops usb_ops = {
	.control = usb_control, .bulk = usb_bulk, .msleep = do_msleep, .now_ms = do_now_ms, .log = do_log,
};

static int usb_open(struct usb_ctx *u)
{
	int r = libusb_init(&u->ctx);

	if (r)
		return r;
	u->h = libusb_open_device_with_vid_pid(u->ctx, TORNE_USB_VENDOR, TORNE_USB_PRODUCT);
	if (!u->h) {
		fprintf(stderr, "CECH-ZD1 J not found\n");
		return -1;
	}
	if (libusb_kernel_driver_active(u->h, 0) == 1)
		libusb_detach_kernel_driver(u->h, 0);
	r = libusb_claim_interface(u->h, 0);
	if (r) {
		fprintf(stderr, "claim interface: %s\n", libusb_error_name(r));
		return r;
	}
	libusb_set_interface_alt_setting(u->h, 0, 0);
	return 0;
}

/* ---- fake memory device ---- */

#define EMU_SLOTS (1u << 18)

struct emu_ctx {
	u32 key[EMU_SLOTS];
	u8 val[EMU_SLOTS];
	u8 used[EMU_SLOTS];
	u32 alive;
	u64 clock;
};

static u8 *emu_slot(struct emu_ctx *e, u32 addr, int create)
{
	u32 h = (addr * 2654435761u) >> 14 & (EMU_SLOTS - 1);

	for (;;) {
		if (!e->used[h]) {
			static u8 zero;

			if (!create) {
				zero = 0;
				return &zero;
			}
			e->used[h] = 1;
			e->key[h] = addr;
			e->val[h] = 0;
			return &e->val[h];
		}
		if (e->key[h] == addr)
			return &e->val[h];
		h = (h + 1) & (EMU_SLOTS - 1);
	}
}

static int emu_control(void *priv, u8 request_type, u16 value, u16 index, void *buf, u16 len)
{
	struct emu_ctx *e = priv;
	u32 addr = (u32)(value >> 8) << 16 | (u32)(value & 0xff) << 24 | index;
	u8 *p = buf;
	u16 i;

	if (request_type == 0xc0) {
		if (addr == 0x1700000 + 0xa3f00 + 0x7c) {
			e->alive++;
			p[0] = e->alive >> 24;
			p[1] = e->alive >> 16;
			p[2] = e->alive >> 8;
			p[3] = e->alive;
			printf("R %08x %d (alive)\n", addr, len);
			return len;
		}
		for (i = 0; i < len; i++)
			p[i] = *emu_slot(e, addr + i, 0);
		printf("R %08x %d\n", addr, len);
		return len;
	}
	for (i = 0; i < len; i++)
		*emu_slot(e, addr + i, 1) = p[i];
	printf("W %08x ", addr);
	for (i = 0; i < len; i++)
		printf("%02x", p[i]);
	printf("\n");
	return len;
}

static int emu_bulk(void *priv, u8 endpoint, void *buf, int len, unsigned int timeout_ms)
{
	(void)priv;
	(void)buf;
	(void)timeout_ms;
	printf("B %02x %d\n", endpoint, len);
	return -EIO;
}

static void emu_msleep(void *priv, unsigned int ms)
{
	struct emu_ctx *e = priv;

	e->clock += ms;
}

static u64 emu_now(void *priv)
{
	return ((struct emu_ctx *)priv)->clock++;
}

static const struct torne_ops emu_ops = {
	.control = emu_control, .bulk = emu_bulk, .msleep = emu_msleep, .now_ms = emu_now, .log = NULL,
};

extern const u32 torne_addressing0_code_for_test[];

static void emu_preload(struct emu_ctx *e)
{
	/* the memory request handler as it is at power-up (the words Start checks before it widens it) */
	static const u32 handler[][2] = {
		{ 0xe9ac0, 0x9b800000 }, { 0xe9ac4, 0xaa04e40f }, { 0xe9e0c, 0x9b800000 },
		{ 0xe9e10, 0xaa04e40d }, { 0xe9d94, 0xb4279f8d },
	};
	unsigned int i, k;

	for (i = 0; i < sizeof(handler) / sizeof(handler[0]); i++)
		for (k = 0; k < 4; k++)
			*emu_slot(e, handler[i][0] + k, 1) = handler[i][1] >> (24 - 8 * k);
}

static int cmd_emu_trace(void)
{
	static struct emu_ctx e;
	struct torne_bridge b;
	int r;

	emu_preload(&e);
	torne_bridge_init(&b, &emu_ops, &e);
	r = torne_bridge_start(&b);
	printf("# Start -> %d\n", r);
	r = torne_bridge_reset_demodulator(&b);
	printf("# ResetDemodulator -> %d\n", r);
	r = torne_bridge_prepare_stream(&b);
	if (!r)
		r = torne_bridge_start_stream(&b);
	printf("# StartStream -> %d\n", r);
	r = torne_bridge_stop_stream(&b);
	printf("# StopStream -> %d\n", r);
	return 0;
}

/* ---- real device ---- */

static int open_board(struct usb_ctx *u, struct torne_bridge *b)
{
	int r = usb_open(u);

	if (r)
		return r;
	torne_bridge_init(b, &usb_ops, u);
	b->log_level = TORNE_LOG_INFO;
	r = torne_bridge_start(b);
	if (r) {
		fprintf(stderr, "start: %d\n", r);
		return r;
	}
	r = torne_bridge_reset_demodulator(b);
	if (r)
		fprintf(stderr, "reset demodulator: %d\n", r);
	return r;
}

static int cmd_probe(void)
{
	struct usb_ctx u = { 0 };
	struct torne_bridge b;
	u8 reg[2] = { 0x03, 0x80 }, val[2];
	int r;
	u64 t0;

	r = open_board(&u, &b);
	if (r)
		return 1;
	r = torne_i2c_write(&b, 0x12, (const u8[]){ 0x05, 0x07, 0x00, 0x03 }, 4);
	fprintf(stderr, "enumerate write: %d\n", r);
	t0 = mono_ms();
	r = torne_i2c_write_read(&b, 0x12, reg, 2, val, 2);
	fprintf(stderr, "DiB8000 register 896: r=%d value=%02x%02x (want 01b3) in %llu ms, %lu requests\n", r, val[0], val[1],
		(unsigned long long)(mono_ms() - t0), u.requests);
	return r != 0;
}

#ifdef WITH_DIB
static int cmd_tune(int ch, int seconds)
{
	struct usb_ctx u = { 0 };
	struct torne_bridge b;
	static struct torne_dib dib_state;
	struct torne_dib *dib = &dib_state;
	int r;

	r = open_board(&u, &b);
	if (r)
		return 1;
	torne_dib_setup(dib, &b);
	r = torne_dib_init(dib);
	fprintf(stderr, "dib init: %d (%lu requests)\n", r, u.requests);
	if (r)
		return 1;
	{
		u64 t0 = mono_ms();
		u32 hz = 473142857u;
		u8 *raw = malloc(65536), *out = malloc(65536);
		static struct torne_framer fr;
		struct torne_dib_status st;
		u64 end;

		/* terrestrial channel n (13..62): 473.143 MHz + 6 MHz * (n - 13) */
		hz = 473142857u + 6000000u * (u32)(ch - 13);
		torne_bridge_prepare_stream(&b);
		r = torne_dib_tune(dib, hz);
		fprintf(stderr, "tune %u Hz: %d (%llu ms)\n", hz, r, (unsigned long long)(mono_ms() - t0));
		if (r)
			return 1;
		torne_dib_status(dib, &st);
		fprintf(stderr, "status: locked=%d cnr=%d (x100 dB) strength=%d (x1000 dBm) mask=%x\n", st.locked, st.cnr_x100,
			st.strength_dbm_x1000, st.status);
		torne_dib_set_output(dib, TORNE_OUT_PARALLEL_GATED);
		r = torne_bridge_start_stream(&b);
		if (r)
			return 1;
		torne_framer_reset(&fr);
		end = mono_ms() + (u64)seconds * 1000;
		while (mono_ms() < end) {
			int got = 0;
			int rr = libusb_bulk_transfer(u.h, 0x81, raw, 61440, &got, 200);

			if (rr && rr != LIBUSB_ERROR_TIMEOUT) {
				fprintf(stderr, "bulk: %s\n", libusb_error_name(rr));
				break;
			}
			if (got > 0) {
				size_t n = torne_framer_feed(&fr, raw, got, out);

				fwrite(out, 1, n, stdout);
			}
		}
		torne_bridge_stop_stream(&b);
		torne_dib_set_output(dib, TORNE_OUT_HIGHZ);
		torne_dib_sleep(dib);
	}
	return 0;
}
#endif

/* card slot test: present, reset (ATR) and a B-CAS "INIT" APDU */
static int cmd_card(void)
{
	struct usb_ctx u = { 0 };
	struct torne_bridge b;
	static struct torne_card card;
	u8 atr[33], resp[260];
	unsigned int atr_len = 0, resp_len = sizeof(resp), i;
	bool present = false;
	int r;
	/* B-CAS: INIT_SETTING_CONDITION */
	static const u8 init_cmd[] = { 0x90, 0x30, 0x00, 0x00, 0x00 };

	r = usb_open(&u);
	if (r)
		return 1;
	torne_bridge_init(&b, &usb_ops, &u);
	b.log_level = TORNE_LOG_INFO;
	r = torne_bridge_start(&b);
	if (r) {
		fprintf(stderr, "start: %d\n", r);
		return 1;
	}
	torne_card_setup(&card, &b);
	r = torne_card_present(&card, &present);
	fprintf(stderr, "present: r=%d %d\n", r, present);
	r = torne_card_reset(&card, atr, &atr_len);
	fprintf(stderr, "reset: r=%d atr_len=%u\nATR:", r, atr_len);
	for (i = 0; r == 0 && i < atr_len; i++)
		fprintf(stderr, " %02X", atr[i]);
	fprintf(stderr, "\n");
	if (r)
		return 1;
	r = torne_card_transmit(&card, init_cmd, sizeof(init_cmd), resp, &resp_len);
	fprintf(stderr, "INIT: r=%d len=%u\n", r, resp_len);
	for (i = 0; r == 0 && i < resp_len; i++)
		fprintf(stderr, "%02X%s", resp[i], (i % 32) == 31 ? "\n" : " ");
	fprintf(stderr, "\n");
	return r != 0;
}
int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "emu-trace"))
		return cmd_emu_trace();
	if (argc >= 2 && !strcmp(argv[1], "card"))
		return cmd_card();
	if (argc >= 2 && !strcmp(argv[1], "probe"))
		return cmd_probe();
#ifdef WITH_DIB
	if (argc >= 3 && !strcmp(argv[1], "tune"))
		return cmd_tune(atoi(argv[2]), argc >= 4 ? atoi(argv[3]) : 10);
#endif
	fprintf(stderr, "usage: %s emu-trace | probe | tune CH [SECONDS]\n", argv[0]);
	return 2;
}
