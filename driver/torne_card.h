/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * torne_card.h - the card slot of the Sony CECH-ZD1 J (APDU level, T=1)
 */
#ifndef TORNE_CARD_H
#define TORNE_CARD_H

#include "torne_core.h"

struct torne_card {
	struct torne_bridge *br;
	bool open; /* a reset produced a usable ATR and the block size has been agreed */
	u8 atr[33];
	unsigned int atr_len;
	u8 send, receive; /* block numbers each end is up to */
	bool use_crc;
	int ifsc; /* the largest information field the card accepts */
	unsigned int block_wait_ms;
	int baudrate;
};

void torne_card_setup(struct torne_card *c, struct torne_bridge *br);
void torne_card_invalidate(struct torne_card *c);
int torne_card_present(struct torne_card *c, bool *present);
/* power-cycles the card, reads the ATR (at most 33 bytes into atr) and agrees a block size; -ENOMEDIUM without a card */
int torne_card_reset(struct torne_card *c, u8 *atr, unsigned int *atr_len);
/* one command APDU; *resp_len is the room on entry and the response length (status bytes included) on return */
int torne_card_transmit(struct torne_card *c, const u8 *apdu, unsigned int apdu_len, u8 *resp, unsigned int *resp_len);

#endif /* TORNE_CARD_H */
