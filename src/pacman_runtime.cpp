/* pacman_runtime.cpp — runtime-loaded replacements for the pre-baked
 * Pac-Man game data (previously in pacman_rom.h / pacman_cmap.h etc.).
 *
 * Each pointer is populated at boot from an in-RAM copy of puckman.zip
 * by runtime_load_pacman(). Before that, the pointers are null — safe
 * to leave in the rom_table initializers because that table is a
 * static local (init on first call) and emulation_task doesn't start
 * running until AFTER setup() completes, by which time load has run.
 */

#include <Arduino.h>
#include "pico_romfs.h"   /* ROM zips: SD card or the Quad ROM drive */
#include <stdint.h>

/* Type shapes match how galagino.cpp / pacman.h index into them.
 * unsigned short (*p)[4] means "pointer to array of 4 shorts", so
 * p[i] returns a 4-short array — same access syntax as pacman_colormap[i]. */
extern "C" {
    unsigned char             *pacman_rom       = nullptr;   /* 16 KB Z80 code */
    unsigned short            (*pacman_5e)[8]   = nullptr;   /* [256][8] tiles */
    unsigned short            (*pacman_colormap)[4] = nullptr;  /* [64][4] */
    unsigned long             (*pacman_sprites)[64][16] = nullptr; /* [4][64][16] */
    signed char               (*pacman_wavetable)[32] = nullptr;   /* [16][32] */
}

/* Loader entry points from mame_rom_loader.cpp. */
extern "C" unsigned char  *load_pacman_rom_from_zip       (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_pacman_cmap_from_zip      (const uint8_t *zip, size_t sz);
extern "C" signed char    *load_pacman_wavetable_from_zip (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_pacman_tilemap_from_zip   (const uint8_t *zip, size_t sz);
extern "C" unsigned long  *load_pacman_spritemap_from_zip (const uint8_t *zip, size_t sz);

/* Slurp a whole file off SD into a malloc'd buffer (duplicate of the
 * helper in mame_rom_loader.cpp — kept local to avoid making that a
 * public API just for this call). Caller frees. */
static bool sd_slurp(const char *path, uint8_t **out, size_t *out_sz) {
    return romfs_slurp(path, out, out_sz);
}

/* Populate the 5 Pac-Man pointers by loading + converting one of the
 * candidate ZIP files off SD. Returns true if all 5 loaded. */
extern "C" bool runtime_load_pacman(void) {
    const char *candidates[] = { "/roms/puckman.zip", nullptr };
    const char *path = nullptr;
    for (int i = 0; candidates[i]; i++) {
        if (romfs_exists(candidates[i])) { path = candidates[i]; break; }
    }
    if (!path) { Serial.println("[pacman] no puckman.zip on SD"); return false; }

    uint8_t *zip = nullptr;
    size_t zip_sz = 0;
    if (!sd_slurp(path, &zip, &zip_sz)) {
        Serial.println("[pacman] slurp failed"); return false;
    }
    Serial.printf("[pacman] loaded %s (%u bytes) — decoding...\n",
                  path, (unsigned)zip_sz);

    pacman_rom       = load_pacman_rom_from_zip(zip, zip_sz);
    pacman_5e        = (unsigned short (*)[8])load_pacman_tilemap_from_zip(zip, zip_sz);
    pacman_colormap  = (unsigned short (*)[4])load_pacman_cmap_from_zip(zip, zip_sz);
    pacman_sprites   = (unsigned long  (*)[64][16])load_pacman_spritemap_from_zip(zip, zip_sz);
    pacman_wavetable = (signed char    (*)[32])load_pacman_wavetable_from_zip(zip, zip_sz);
    free(zip);

    bool ok = pacman_rom && pacman_5e && pacman_colormap
           && pacman_sprites && pacman_wavetable;
    Serial.printf("[pacman] runtime load %s\n", ok ? "OK" : "FAILED");
    return ok;
}
