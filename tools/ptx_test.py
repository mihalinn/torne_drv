#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""/dev/tornevideoN を px4_drv と同じ ioctl で使うテスト: ptx_test.py DEV CHANNEL SECONDS OUTFILE"""
import fcntl
import os
import struct
import sys
import time

PTX_SET_CHANNEL = 0x40088d01
PTX_START_STREAMING = 0x8d02
PTX_STOP_STREAMING = 0x8d03
PTX_GET_CNR = 0x80088d04

dev, ch, secs, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
fd = os.open(dev, os.O_RDONLY)
t0 = time.time()
# UHF 13-62ch -> freq_no = ch + 50 (63..112)
fcntl.ioctl(fd, PTX_SET_CHANNEL, struct.pack('ii', ch + 50, 0))
print("tuned in %.2fs" % (time.time() - t0), flush=True)
fcntl.ioctl(fd, PTX_START_STREAMING)
end = time.time() + secs
total = 0
with open(out, 'wb') as f:
    while time.time() < end:
        d = os.read(fd, 188 * 1024)
        if not d:
            break
        f.write(d)
        total += len(d)
buf = bytearray(4)
try:
    fcntl.ioctl(fd, PTX_GET_CNR, buf)
    print("cnr raw (0.01dB):", struct.unpack('I', bytes(buf))[0])
except OSError as e:
    print("GET_CNR:", e)
fcntl.ioctl(fd, PTX_STOP_STREAMING)
os.close(fd)
print("read %d bytes (%.2f Mbps)" % (total, total * 8 / secs / 1e6))
