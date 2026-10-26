/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * torne_core.h - Sony CECH-ZD1 J ("torne") driver core
 *
 * The core is OS independent: it talks to the USB device and to the clock
 * through the callbacks in struct torne_ops, so the same code runs in a
 * user-space test program (libusb) and in the kernel module.
 *
 * The CECH-ZD1 J is a Fujitsu MB86H47A USB bridge in front of a DiBcom DiB8096
 * (DiB8000 ISDB-T demodulator + DiB0090 tuner). The protocol was worked out by
 * Till0196's "coax" (Apache-2.0 OR GPL-2.0-or-later, mb86h47 package) and the
 * demodulator/tuner part is a port of the Linux dib8000/dib0090 drivers.
 */
#ifndef TORNE_CORE_H
#define TORNE_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#include <linux/errno.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#endif

#define TORNE_USB_VENDOR 0x054c
#define TORNE_USB_PRODUCT 0x04b2

#define TORNE_TS_PACKET_SIZE 188
#define TORNE_SOURCE_PACKET_SIZE 192 /* 4-byte arrival timestamp + TS packet */

/* The host's view of the device: USB transfers, time, a log. */
struct torne_ops {
	/* vendor request 0xbc, buffer up to 64 bytes. Returns the length or -errno. */
	int (*control)(void *priv, u8 request_type, u16 value, u16 index, void *buf, u16 len);
	/* bulk transfer on an endpoint; returns the length or -errno (-ETIMEDOUT). */
	int (*bulk)(void *priv, u8 endpoint, void *buf, int len, unsigned int timeout_ms);
	void (*msleep)(void *priv, unsigned int ms);
	u64 (*now_ms)(void *priv); /* monotonic milliseconds */
	void (*log)(void *priv, int level, const char *fmt, ...);
};

#define TORNE_LOG_ERR 0
#define TORNE_LOG_INFO 1
#define TORNE_LOG_DEBUG 2

struct torne_bridge {
	const struct torne_ops *ops;
	void *priv;

	int err;	 /* first error of a sequence of accesses (negative errno) */
	u8 sequence;	 /* last mailbox sequence number */
	bool core0_loaded;
	bool streaming;
	int log_level;
};

/* ---- bridge (MB86H47A) ---- */
void torne_bridge_init(struct torne_bridge *b, const struct torne_ops *ops, void *priv);
int torne_bridge_start(struct torne_bridge *b);
int torne_bridge_reset_demodulator(struct torne_bridge *b);
/* raw 32-bit chip memory access (the card slot's UART registers); errors are kept in b->err */
u32 torne_mem_read32(struct torne_bridge *b, u32 address);
void torne_mem_write32(struct torne_bridge *b, u32 address, u32 value);
/* I2C master (8-bit slave addresses) */
int torne_i2c_write(struct torne_bridge *b, u8 address, const u8 *data, unsigned int len);
int torne_i2c_read(struct torne_bridge *b, u8 address, u8 *data, unsigned int len);
int torne_i2c_write_read(struct torne_bridge *b, u8 address, const u8 *wdata, unsigned int wlen, u8 *rdata,
			 unsigned int rlen);
/* stream */
int torne_bridge_prepare_stream(struct torne_bridge *b);
int torne_bridge_start_stream(struct torne_bridge *b);
int torne_bridge_stop_stream(struct torne_bridge *b);

/* turns 192-byte units (timestamp + packet) into 188-byte TS packets; a unit may straddle two reads */
#define TORNE_FRAMER_CAP 32768
struct torne_framer {
	u8 buf[TORNE_FRAMER_CAP];
	size_t len;
};
void torne_framer_reset(struct torne_framer *f);
/* appends whole TS packets to out (room for raw_len / 192 * 188 + 188 bytes); returns bytes written */
size_t torne_framer_feed(struct torne_framer *f, const u8 *raw, size_t raw_len, u8 *out);
/* stateless: takes whole units off data, returns the bytes used */
size_t torne_ts_extract(const u8 *data, size_t len, u8 *out, size_t *written);
#endif /* TORNE_CORE_H */
