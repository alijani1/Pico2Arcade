/* z80_galagino.h — Galagino-facing interface for the superzazu Z80 core.
 *
 * Provides the same names the game code used with the previous core
 * (Z80 type, StepZ80/IntZ80/ResetZ80, INT_* vectors, byte/word) on top of
 * zazu_z80.c, so the per-game files don't change. Selected by
 * -DZ80_CORE_ZAZU (see z80_core.h / platformio.ini).
 *
 * Interrupt model (matches what the games were tuned against):
 *   IntZ80() acts IMMEDIATELY — a maskable interrupt is taken if IFF1 is set
 *   and otherwise dropped (not queued); NMI is always taken. A HALTed CPU is
 *   released even when the interrupt itself is dropped.
 */
#ifndef Z80_GALAGINO_H
#define Z80_GALAGINO_H

#include "zazu_z80.h"

typedef unsigned char  byte;
typedef unsigned short word;
typedef signed char    offset;

typedef z80 Z80;

#define INT_RST00   0x00C7     /* RST 00h */
#define INT_RST08   0x00CF     /* RST 08h */
#define INT_RST10   0x00D7     /* RST 10h */
#define INT_RST18   0x00DF     /* RST 18h */
#define INT_RST20   0x00E7     /* RST 20h */
#define INT_RST28   0x00EF     /* RST 28h */
#define INT_RST30   0x00F7     /* RST 30h */
#define INT_RST38   0x00FF     /* RST 38h */
#define INT_IRQ     INT_RST38
#define INT_NMI     0xFFFD     /* Non-maskable interrupt */
#define INT_NONE    0xFFFF
#define INT_QUIT    0xFFFE

/* Core-neutral register access for game code. */
#define Z80_IFF1(R) ((R)->iff1)
#define Z80_PC(R)   ((R)->pc)

#ifdef __cplusplus
extern "C" {
#endif
void ResetZ80(Z80 *R);
void StepZ80(Z80 *R);
void digdug_StepZ80(Z80 *R);
void IntZ80(Z80 *R, word Vector);

/* Implemented by the emulator (emulation.c). */
void WrZ80(word Addr, byte Value);
byte RdZ80(word Addr);
void OutZ80(word Port, byte Value);
byte InZ80(word Port);
void PatchZ80(Z80 *R);
#ifdef __cplusplus
}
#endif

#include "config.h"
#include "emulation.h"

#endif
