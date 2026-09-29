"""Summarise F-register differences between two FULL (flags included)
z80trace files: how many records differ only in F, grouped by the PC of
the instruction, with the first example of each.

    python flagdiff.py before.bin after.bin
"""
import struct
import sys
from collections import Counter

FMT = "<BBHHHHHHHHBB"
SZ = struct.calcsize(FMT)
CHUNK = SZ * 65536

by_pc, example, total, other = Counter(), {}, 0, 0
with open(sys.argv[1], "rb") as fa, open(sys.argv[2], "rb") as fb:
    idx = 0
    while True:
        ba, bb = fa.read(CHUNK), fb.read(CHUNK)
        if not ba:
            break
        if ba == bb:
            idx += len(ba) // SZ
            continue
        for ra, rb in zip(struct.iter_unpack(FMT, ba), struct.iter_unpack(FMT, bb)):
            if ra != rb:
                la, lb = list(ra), list(rb)
                la[3] &= 0xFF00; lb[3] &= 0xFF00
                if la != lb:
                    other += 1
                else:
                    key = (ra[0], ra[2])
                    by_pc[key] += 1
                    example.setdefault(key, (idx, ra[3] & 0xFF, rb[3] & 0xFF))
                total += 1
            idx += 1
print(f"records differing: {total}  (non-flag: {other})")
for (cpu, pc), n in by_pc.most_common(20):
    i, fa_, fb_ = example[(cpu, pc)]
    print(f"cpu{cpu} pc={pc:04x}  {n:9d} records  first@{i}  F A={fa_:02x} B={fb_:02x}")
