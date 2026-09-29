"""Instruction mix of a z80trace run: counts per opcode (with ED/CB/DD/FD
prefixes resolved from the ROMs) and the PC range per CPU, to see where
emulation time goes.

    python histogram.py trace.bin rom_dir [top]
"""
import struct
import sys
from collections import Counter

FMT = "<BBHHHHHHHHBB"
SZ = struct.calcsize(FMT)
roms = [open(f"{sys.argv[2]}/galaga_cpu{i}.bin", "rb").read() for i in (1, 2, 3)]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25

ops, kinds, pcmax = Counter(), Counter(), [0, 0, 0]
with open(sys.argv[1], "rb") as f:
    while True:
        chunk = f.read(SZ * 65536)
        if not chunk:
            break
        for cpu, kind, pc, *_ in struct.iter_unpack(FMT, chunk):
            kinds[kind] += 1
            if kind:
                continue
            rom = roms[cpu]
            pcmax[cpu] = max(pcmax[cpu], pc)
            if pc >= len(rom):
                ops["<RAM>"] += 1
                continue
            b = rom[pc]
            if b in (0xED, 0xCB, 0xDD, 0xFD) and pc + 1 < len(rom):
                key = f"{b:02X} {rom[pc + 1]:02X}"
            else:
                key = f"{b:02X}"
            ops[key] += 1

total = sum(ops.values())
print(f"instructions: {total}  interrupts: {kinds[1]}  nmi: {kinds[2]}")
print("max PC per cpu:", [hex(p) for p in pcmax], "(ROM sizes 4000/1000/1000)")
acc = 0
for k, n in ops.most_common(top):
    acc += n
    print(f"{k:8s} {n:10d} {100*n/total:5.1f}%  cum {100*acc/total:5.1f}%")
