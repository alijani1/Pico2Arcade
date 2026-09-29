#!/bin/sh
# Builds trace_marat and trace_zazu with a native gcc. Only the Galaga
# machine is compiled (GAL_ONLY_GALAGA); emulation.c's StepZ80/IntZ80 calls
# are redirected to the harness's tracing wrappers.
#
# On Windows use WSL (MinGW's ld crashes linking the old Z80 core here):
#   wsl -e sh tools/z80trace/build.sh /tmp/z80trace
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT"
SRC="$HERE/../../src"
COMMON="-O2 -w -DGAL_ONLY_GALAGA -I$HERE/stub -I$SRC"
EMU_RENAME="-DStepZ80=trace_StepZ80 -DIntZ80=trace_IntZ80"

gcc $COMMON $EMU_RENAME -c "$SRC/emulation.c" -o "$OUT/emu_marat.o"
gcc $COMMON -c "$SRC/Z80.c" -o "$OUT/core_marat.o"
gcc $COMMON -c "$HERE/harness.c" -o "$OUT/harness_marat.o"
gcc "$OUT/emu_marat.o" "$OUT/core_marat.o" "$OUT/harness_marat.o" -o "$OUT/trace_marat"

ZAZU="-DZ80_CORE_ZAZU -DZ80_ZAZU_GALAGINO"
gcc $COMMON $ZAZU $EMU_RENAME -c "$SRC/emulation.c" -o "$OUT/emu_zazu.o"
gcc $COMMON $ZAZU -c "$SRC/z80_zazu/zazu_z80.c" -o "$OUT/core_zazu.o"
gcc $COMMON $ZAZU -c "$SRC/z80_zazu/z80_galagino.c" -o "$OUT/adapter_zazu.o"
gcc $COMMON $ZAZU -c "$HERE/harness.c" -o "$OUT/harness_zazu.o"
gcc "$OUT/emu_zazu.o" "$OUT/core_zazu.o" "$OUT/adapter_zazu.o" "$OUT/harness_zazu.o" -o "$OUT/trace_zazu"
echo "built into $OUT"
