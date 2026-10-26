#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""TS ファイルの同期ずれ・巡回カウンタ(CC)の飛び・暗号化パケットを数える"""
import sys

d = open(sys.argv[1], 'rb').read()
n = len(d) // 188
bad = sum(1 for i in range(n) if d[i * 188] != 0x47)
last = {}
cc_err = 0
pids = {}
scr = 0
for i in range(n):
    p = d[i * 188:(i + 1) * 188]
    pid = ((p[1] & 0x1f) << 8) | p[2]
    pids[pid] = pids.get(pid, 0) + 1
    if (p[3] >> 6) & 3:
        scr += 1
    afc = (p[3] >> 4) & 3
    cc = p[3] & 15
    if pid == 0x1fff or (p[1] & 0x80):
        continue
    if afc in (1, 3):
        pv = last.get(pid)
        if pv is not None and cc != pv and cc != (pv + 1) & 15:
            cc_err += 1
        last[pid] = cc
print("bytes", len(d), "packets", n, "tail", len(d) % 188, "sync-bad", bad, "cc_err", cc_err, "scrambled", scr, "pids", len(pids))
print("top", sorted(pids.items(), key=lambda x: -x[1])[:5])
