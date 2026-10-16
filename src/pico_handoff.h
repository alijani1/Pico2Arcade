/* pico_handoff.h — state passed between PicoCartReader and Pico2Arcade
 * across the reboot that switches firmwares (arcade mode).
 *
 * Uses RP2350 watchdog scratch registers, which survive a watchdog
 * reboot and the boot ROM's chain_image, and are cleared by power-on.
 * MUST stay in sync with PicoCartReader/src/arcade.h:
 *   scratch[0] = ARCADE magic -> CartReader chain-boots us on next boot
 *   scratch[1] = VOLUME tag | pct (0..100), written by both sides
 */
#ifndef PICO_HANDOFF_H
#define PICO_HANDOFF_H

#ifdef __cplusplus
extern "C" {
#endif

#define HANDOFF_SCRATCH_ARCADE 0
#define HANDOFF_ARCADE_MAGIC   0xA7CADE01u
#define HANDOFF_SCRATCH_VOLUME 1
#define HANDOFF_VOLUME_TAG     0x564F0000u
/* Quad Ultimate CX host only (GameAdapterRGBPico include/arcade.hpp): where
 * its FAT ROM drive sits inside our chain-boot window, in 4 KB units. */
#define HANDOFF_SCRATCH_FATOFS  2
#define HANDOFF_FATOFS_TAG      0xFA700000u
#define HANDOFF_SCRATCH_FATSIZE 3
#define HANDOFF_FATSIZE_TAG     0xFA710000u
/* Quad host: scratch[0] (free once it has chain-booted us) = this tag | the
 * ROM drive's PHYSICAL flash offset in 4 KB units — the chain window maps it
 * for reads only, so USB ROM drive mode needs it to write the drive. */
#define HANDOFF_SCRATCH_HOST    0
#define HANDOFF_HOST_QUAD_TAG   0x51AD0000u

/* scratch[1] bits 8..15 = option flags, carried both ways with the volume
 * (Quad host: saved in its settings, so they survive power-off). */
#define HANDOFF_FLAG_PORTRAIT  0x01u   /* display portrait (else landscape) */
#define HANDOFF_FLAG_DRIVE     0x02u   /* next boot: USB ROM drive mode (cleared on entry) */

/* Volume handed over by the CartReader (0..100), or -1 if none. */
int  handoff_volume_read(void);
/* Option flags from the host (HANDOFF_FLAG_*), or -1 if none. */
int  handoff_flags_read(void);
/* Publish option flags (kept alongside the volume) for the next boot/host. */
void handoff_flags_write(int flags);
/* Publish the current volume so the CartReader adopts it on return. */
void handoff_volume_write(int pct);
/* Reboot straight back into this firmware's game list. Under the
 * CartReader, sets the arcade magic so it chain-boots us again; when
 * running standalone the reboot lands in our menu anyway. */
void handoff_relaunch_arcade(void);
/* Reboot to the CartReader (standalone: just restarts us). */
void handoff_exit_to_cartreader(void);

#ifdef __cplusplus
}
#endif

#endif
