/* pico_romfs.h — where the game ROM zips come from.
 *
 * Default (PicoCartReader): the SD card, /roms/<set>.zip.
 * PICO_QUAD / PICO_5200 (hosted: Quad Ultimate CX, Duo 5200 Ultimate): the
 *   host's 256 KB FAT12 "ARCADE ROMS" drive (early Quad drives: "QUAD ROMS") in
 *   flash. The Quad chain-boots us with the boot ROM's rom_chain_image(); the
 *   window it maps to 0x10000000 contains this image followed by the ROM drive,
 *   so the drive is plain memory at 0x10000000 + its window offset. The Quad
 *   passes offset/size in watchdog scratch[2]/[3] (see pico_handoff.h); build
 *   defaults are used if they are missing. Read via XIP; the Quad writes the drive
 *   (Mini-Games > Arcade ROM Drive), as does our own USB ROM drive mode
 *   (it gets the physical offset from the Quad in scratch[0]).
 *
 * PICO_STANDALONE (plain Pico 2): our own FAT12 "ARCADE ROMS" drive at 2 MB
 *   (1 MB), read the same way (XIP maps it 1:1).
 * On both flash boards the drive is written only in USB ROM drive mode
 * (pico_drive.cpp), which formats it on first use.
 *
 * Paths are the SD-style "/roms/<name>.zip"; on the flash drives the zip is
 * looked up by name in the root directory, then in an ARCADE, MAME, ROMS,
 * ROMS/ARCADE or ROMS/MAME folder (shared drives with per-project folders).
 * The SD card is searched the same way, starting with /roms/.
 */
#ifndef PICO_ROMFS_H
#define PICO_ROMFS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the ROM storage (SD init / locate + validate the flash drive). */
bool romfs_init(void);
/* True if the ROM zip at `path` (e.g. "/roms/galaga.zip") is present. */
bool romfs_exists(const char *path);
/* Read the whole file into a malloc'd buffer (caller frees). */
bool romfs_slurp(const char *path, uint8_t **out, size_t *out_sz);
/* Short human description of where ROMs go, for menu/error text. */
const char *romfs_where(void);
/* Flash boards only: the drive's XIP address, physical flash offset and size
 * (formatted or not) — for USB ROM drive mode. */
void romfs_location(const uint8_t **xip, uint32_t *phys, uint32_t *size);

#ifdef PICO_SD
/* The Fruit Jam's SD card (pico_romfs.cpp, as Pico2Atari8): true if the games
 * come from a card (one was in at start-up). */
bool romfs_on_card(void);
/* USB drive mode with a card: its sectors as they are (sector count, 0 if
 * none), read / write n 512-byte sectors, sync. */
uint32_t romfs_card_raw_open(void);
bool romfs_card_raw_read(uint32_t lba, void *buf, uint32_t n);
bool romfs_card_raw_write(uint32_t lba, const void *buf, uint32_t n);
void romfs_card_raw_sync(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
