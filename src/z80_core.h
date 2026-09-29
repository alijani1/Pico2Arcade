/* z80_core.h — selects the Z80 CPU core at build time.
 *
 *   -DZ80_CORE_ZAZU  superzazu/z80 (MIT) via z80_zazu/z80_galagino.h
 *   (default)        previous core (Z80.h) — NOT licensed for commercial
 *                    use; kept only until the replacement is validated.
 *
 * Game code must use the core-neutral accessors below instead of
 * touching core-specific register fields. */
#ifndef Z80_CORE_H
#define Z80_CORE_H

#ifdef Z80_CORE_ZAZU
#include "z80_zazu/z80_galagino.h"
#else
#include "Z80.h"
#define Z80_IFF1(R) ((R)->IFF & IFF_1)
#define Z80_PC(R)   ((R)->PC.W)
#endif

#endif
