"""Find and show the first differing record between two z80trace files.

    python compare.py before.bin after.bin [context] [--no-flags]

--no-flags ignores the F register, to separate flag-only differences
(e.g. a core computing P/V or N differently) from real divergence in
program flow or data registers.
"""
import struct
import sys

FMT = "<BBHHHHHHHHBB"
SZ = struct.calcsize(FMT)
KIND = {0: "op ", 1: "INT", 2: "NMI"}


def recs(path):
    data = open(path, "rb").read()
    return [struct.unpack_from(FMT, data, i) for i in range(0, len(data) - SZ + 1, SZ)]


def fmt(r):
    cpu, kind, pc, af, bc, de, hl, sp, ix, iy, iff1, _ = r
    return (f"cpu{cpu} {KIND.get(kind,'?')} pc={pc:04x} | AF={af:04x} BC={bc:04x} "
            f"DE={de:04x} HL={hl:04x} SP={sp:04x} IX={ix:04x} IY={iy:04x} IFF1={iff1}")


args = [x for x in sys.argv[1:] if not x.startswith("--")]
no_flags = "--no-flags" in sys.argv
a, b = recs(args[0]), recs(args[1])
ctx = int(args[2]) if len(args) > 2 else 6


def key(r):
    if not no_flags:
        return r
    r = list(r)
    r[3] &= 0xFF00          # drop F, keep A
    return tuple(r)


first = next((i for i, (x, y) in enumerate(zip(a, b)) if key(x) != key(y)), None)
if first is None:
    print(f"identical for {min(len(a), len(b))} records")
    sys.exit(0)
print(f"first difference at record {first}\n")
for i in range(max(0, first - ctx), min(len(a), first + 3)):
    mark = ">>" if i == first else "  "
    print(f"{mark} {i:8d} A {fmt(a[i])}")
    if key(a[i]) != key(b[i]) or a[i] != b[i]:
        print(f"{mark} {i:8d} B {fmt(b[i])}")
