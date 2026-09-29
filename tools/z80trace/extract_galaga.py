"""Extract Galaga's three CPU ROMs from a MAME galaga.zip exactly the way
PicoGalagino's loader does (mame_rom_loader.cpp): cpu1 = 4 chips
concatenated + the 5 boot patches, cpu2 = gg1_5b.3f, cpu3 = gg1_7b.2c.

    python extract_galaga.py path/to/galaga.zip out_dir
"""
import os
import sys
import zipfile

PATCHES = [  # offset, expect, replace — keep in sync with mame_rom_loader.cpp
    (0x3382, 0x06, 0xC3),
    (0x3383, 0x0A, 0x35),
    (0x3384, 0xD9, 0x34),
    (0x348A, 0x1E, 0x01),
    (0x352B, 0xE5, 0xC9),
]

zip_path, out_dir = sys.argv[1], sys.argv[2]
os.makedirs(out_dir, exist_ok=True)
with zipfile.ZipFile(zip_path) as z:
    cpu1 = bytearray(b"".join(z.read(n) for n in
                              ["gg1_1b.3p", "gg1_2b.3m", "gg1_3.2m", "gg1_4b.2l"]))
    for off, expect, replace in PATCHES:
        if cpu1[off] != expect:
            print(f"warning: patch @{off:#x} expected {expect:02x} got {cpu1[off]:02x}")
        cpu1[off] = replace
    open(os.path.join(out_dir, "galaga_cpu1.bin"), "wb").write(cpu1)
    open(os.path.join(out_dir, "galaga_cpu2.bin"), "wb").write(z.read("gg1_5b.3f"))
    open(os.path.join(out_dir, "galaga_cpu3.bin"), "wb").write(z.read("gg1_7b.2c"))
print("ok")
