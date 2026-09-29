#!/bin/sh
# Builds trace_zazu with a native gcc: Galagino's Galaga machine
# (GAL_ONLY_GALAGA) on the superzazu Z80 core, with emulation.c's
# StepZ80/IntZ80 calls redirected to the harness's tracing wrappers.
#
# On Windows use WSL (MinGW's ld crashed on this code here):
#   wsl -e sh tools/z80trace/build.sh ~/z80trace
#
# To check that a core change doesn't alter behaviour, build and trace
# before and after the change, then compare the two traces with
# compare.py (--no-flags to ignore F) or flagdiff.py.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT"
SRC="$HERE/../../src"
COMMON="-O2 -w -DGAL_ONLY_GALAGA -DZ80_CORE_ZAZU -DZ80_ZAZU_GALAGINO -I$HERE/stub -I$SRC"
EMU_RENAME="-DStepZ80=trace_StepZ80 -DIntZ80=trace_IntZ80"

gcc $COMMON $EMU_RENAME -c "$SRC/emulation.c" -o "$OUT/emu.o"
gcc $COMMON -c "$SRC/z80_zazu/zazu_z80.c" -o "$OUT/core.o"
gcc $COMMON -c "$SRC/z80_zazu/z80_galagino.c" -o "$OUT/adapter.o"
gcc $COMMON -c "$HERE/harness.c" -o "$OUT/harness.o"
gcc "$OUT/emu.o" "$OUT/core.o" "$OUT/adapter.o" "$OUT/harness.o" -o "$OUT/trace_zazu"
echo "built $OUT/trace_zazu"
