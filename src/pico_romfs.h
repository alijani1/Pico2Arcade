/* pico_romfs.h — where the game ROM zips come from.
 *
 * Default (PicoCartReader): the SD card, /roms/<set>.zip.
 * PICO_QUAD (Quad Ultimate CX): the Quad's 256 KB FAT12 "QUAD ROMS" drive in
 *   flash. The Quad chain-boots us with the boot ROM's rom_chain_image(); the
 *   window it maps to 0x10000000 contains this image followed by the ROM drive,
 *   so the drive is plain memory at 0x10000000 + its window offset. The Quad
 *   passes offset/size in watchdog scratch[2]/[3] (see pico_handoff.h); build
 *   defaults are used if they are missing. Read-only — the Quad writes the drive
 *   (Mini-Games > Arcade ROM Drive), we never do.
 *
 * Paths are the SD-style "/roms/<name>.zip"; on the Quad drive the zip is
 * looked up by name in the root directory, or in a ROMS folder.
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

#ifdef __cplusplus
}
#endif

#endif
