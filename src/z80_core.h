/* z80_core.h — the Z80 CPU core used by the emulator.
 *
 * superzazu/z80 (MIT) with Pico2Arcade patches, via the adapter in
 * z80_zazu/z80_galagino.h (Z80 type, StepZ80/IntZ80/ResetZ80, INT_*).
 * Built with -DZ80_CORE_ZAZU -DZ80_ZAZU_GALAGINO (platformio.ini).
 *
 * Game code must use the core-neutral accessors (Z80_IFF1, Z80_PC)
 * instead of touching core-specific register fields. */
#ifndef Z80_CORE_H
#define Z80_CORE_H

#ifndef Z80_CORE_ZAZU
#error "Build with -DZ80_CORE_ZAZU -DZ80_ZAZU_GALAGINO (see platformio.ini)"
#endif

#include "z80_zazu/z80_galagino.h"

#endif
