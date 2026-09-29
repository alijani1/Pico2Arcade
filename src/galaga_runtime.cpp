/* galaga_runtime.cpp — Galaga runtime-loaded pointers + loader.
 * Same pattern as pacman_runtime.cpp. */

#include <Arduino.h>
#include "pico_romfs.h"   /* ROM zips: SD card or the Quad ROM drive */
#include <stdint.h>

extern "C" {
    unsigned char  *galaga_rom_cpu1 = nullptr;
    unsigned char  *galaga_rom_cpu2 = nullptr;
    unsigned char  *galaga_rom_cpu3 = nullptr;
    unsigned short (*gg1_9_4l)[8]               = nullptr; /* tilemap: array name auto-derived from ROM filename */
    unsigned long  (*galaga_sprites)[128][16]   = nullptr; /* [4 flip variants][128 sprites][16 rows] */
    unsigned short (*galaga_colormap_tiles)[4]  = nullptr;
    unsigned short (*galaga_colormap_sprites)[4] = nullptr;
    signed char    (*galaga_wavetable)[32]      = nullptr;
}

/* Loader entry points from mame_rom_loader.cpp. */
extern "C" unsigned char  *load_galaga_rom_cpu1_from_zip     (const uint8_t *zip, size_t sz);
extern "C" unsigned char  *load_galaga_rom_cpu2_from_zip     (const uint8_t *zip, size_t sz);
extern "C" unsigned char  *load_galaga_rom_cpu3_from_zip     (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_galaga_tilemap_from_zip      (const uint8_t *zip, size_t sz);
extern "C" unsigned long  *load_galaga_spritemap_from_zip    (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_galaga_cmap_tiles_from_zip   (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_galaga_cmap_sprites_from_zip (const uint8_t *zip, size_t sz);
extern "C" signed char    *load_galaga_wavetable_from_zip    (const uint8_t *zip, size_t sz);

static bool sd_slurp(const char *path, uint8_t **out, size_t *out_sz) {
    return romfs_slurp(path, out, out_sz);
}

extern "C" bool runtime_load_galaga(void) {
    const char *path = "/roms/galaga.zip";
    if (!romfs_exists(path)) { Serial.println("[galaga] galaga.zip not on SD"); return false; }

    uint8_t *zip = nullptr; size_t zip_sz = 0;
    if (!sd_slurp(path, &zip, &zip_sz)) { Serial.println("[galaga] slurp failed"); return false; }
    Serial.printf("[galaga] loaded %s (%u bytes) — decoding...\n", path, (unsigned)zip_sz);

    galaga_rom_cpu1         = load_galaga_rom_cpu1_from_zip(zip, zip_sz);
    galaga_rom_cpu2         = load_galaga_rom_cpu2_from_zip(zip, zip_sz);
    galaga_rom_cpu3         = load_galaga_rom_cpu3_from_zip(zip, zip_sz);
    gg1_9_4l                = (unsigned short (*)[8])load_galaga_tilemap_from_zip(zip, zip_sz);
    galaga_sprites          = (unsigned long  (*)[128][16])load_galaga_spritemap_from_zip(zip, zip_sz);
    galaga_colormap_tiles   = (unsigned short (*)[4])load_galaga_cmap_tiles_from_zip(zip, zip_sz);
    galaga_colormap_sprites = (unsigned short (*)[4])load_galaga_cmap_sprites_from_zip(zip, zip_sz);
    galaga_wavetable        = (signed char    (*)[32])load_galaga_wavetable_from_zip(zip, zip_sz);
    free(zip);

    bool ok = galaga_rom_cpu1 && galaga_rom_cpu2 && galaga_rom_cpu3
           && gg1_9_4l && galaga_sprites
           && galaga_colormap_tiles && galaga_colormap_sprites
           && galaga_wavetable;
    Serial.printf("[galaga] runtime load %s\n", ok ? "OK" : "FAILED");
    return ok;
}

/* Panic if the game boot proceeds with null ROMs — better than a
 * silent black screen. Called on the first CPU fetch when we detect
 * unpopulated pointers. */
extern "C" void galaga_missing_rom_panic(void) {
    Serial.println("[galaga] FATAL: galaga_rom_cpu* is NULL — SD load failed");
    Serial.println("[galaga] Check that /roms/galaga.zip exists on the SD card");
    while (1) { delay(1000); }
}
