# SPDX-License-Identifier: GPL-2.0-only
# torne_user: the driver core (bridge, DiB8096, card) run from user space with libusb, for testing.
# Unload the kernel module first (sudo rmmod torne_drv): both cannot own the device at once.

CC ?= gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -I../driver -DWITH_DIB
SRC = torne_user.c ../driver/torne_bridge.c ../driver/torne_card.c ../driver/torne_dib.c

torne_user: $(SRC) $(wildcard ../driver/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lusb-1.0

clean:
	rm -f torne_user

.PHONY: clean
