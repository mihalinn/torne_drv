# SPDX-License-Identifier: GPL-2.0-only
# pcscd reader driver for the torne's card slot (needs libpcsclite-dev)

CC ?= gcc

libifd_torne.so: ifd_torne.c ../driver/torne_ioctl.h
	$(CC) -shared -fPIC -O2 -Wall -I../driver -I/usr/include/PCSC -o $@ ifd_torne.c

clean:
	rm -f libifd_torne.so

.PHONY: clean
