/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * torne_ioctl.h - user-space interface of the torne driver's card slot (/dev/tornecardN)
 *
 * The CECH-ZD1 J's built-in card slot (for the B-CAS card) is not a CCID reader. The driver speaks ISO 7816-3 T=1 to
 * the card itself and offers APDU-level calls; the pcscd driver in ifd-torne uses them.
 */
#ifndef TORNE_IOCTL_H
#define TORNE_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define TORNE_CARD_MAX_ATR 33
#define TORNE_CARD_MAX_APDU 260

struct torne_card_atr {
	__u32 len;
	__u8 atr[40];
};

struct torne_card_apdu {
	__u32 tx_len;	  /* command APDU length */
	__u32 rx_len;	  /* in: room of rx; out: response length (status bytes included) */
	__u8 tx[TORNE_CARD_MAX_APDU];
	__u8 rx[TORNE_CARD_MAX_APDU];
};

/* returns 1 when a card is seated, 0 when not */
#define TORNE_CARD_IOC_PRESENT _IOR('T', 0x10, int)
/* power-cycles the card and returns its answer to reset (-ENOMEDIUM without a card) */
#define TORNE_CARD_IOC_RESET _IOR('T', 0x11, struct torne_card_atr)
/* sends one command APDU */
#define TORNE_CARD_IOC_TRANSMIT _IOWR('T', 0x12, struct torne_card_apdu)

#endif /* TORNE_IOCTL_H */
