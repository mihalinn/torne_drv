#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""/dev/tornecardN の ioctl を直接たたく: 有無 → リセット(ATR) → B-CAS の INIT APDU"""
import fcntl
import os
import struct
import sys

dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/tornecard0"
IOC_PRESENT = 0x80045410
IOC_RESET = 0x802c5411
IOC_TRANSMIT = 0xc2105412

fd = os.open(dev, os.O_RDWR)

buf = bytearray(4)
fcntl.ioctl(fd, IOC_PRESENT, buf)
print("present:", struct.unpack("i", bytes(buf))[0])

buf = bytearray(44)
fcntl.ioctl(fd, IOC_RESET, buf)
n = struct.unpack_from("I", buf, 0)[0]
print("ATR(%d): %s" % (n, bytes(buf[4:4 + n]).hex(" ").upper()))

for cmd in ("9030000000", "90320000" + "00"):
    tx = bytes.fromhex(cmd)
    req = bytearray(528)
    struct.pack_into("II", req, 0, len(tx), 260)
    req[8:8 + len(tx)] = tx
    try:
        fcntl.ioctl(fd, IOC_TRANSMIT, req)
    except OSError as e:
        print("APDU %s -> error %s" % (cmd, e))
        continue
    rx_len = struct.unpack_from("I", req, 4)[0]
    print("APDU %s -> %d bytes: %s" % (cmd, rx_len, bytes(req[268:268 + rx_len]).hex(" ").upper()))
os.close(fd)
