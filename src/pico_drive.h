/* pico_drive.h — USB ROM drive mode (Quad + standalone Pico 2).
 *
 * The ROM drive (pico_romfs.cpp) shows up on the PC as a USB mass-storage
 * drive: copy the MAME ROM zips (and, standalone, edit ARCADE.INI), eject,
 * press MENU (or START) — we reboot into the game list. Formats the drive
 * (FAT12) on first use; standalone also writes a commented default
 * ARCADE.INI then.
 *
 * Entered on the next boot after pico_drive_request() (menu row "ROM Drive
 * (USB)"), or standalone when MENU is held at power-on or the drive isn't
 * formatted yet. Runs before the game menu, with core 1 still parked.
 */
#ifndef PICO_DRIVE_H
#define PICO_DRIVE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Should this boot be ROM drive mode? (Clears the request.) Call after
 * romfs_init() + pico_input_init(). */
bool pico_drive_requested(void);
/* Reboot into ROM drive mode. */
void pico_drive_request(void);
/* Run ROM drive mode; never returns (reboots into the game list). */
void pico_drive_run(void);

#ifdef __cplusplus
}
#endif

#endif
