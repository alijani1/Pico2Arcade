/* xevious_runtime.cpp — runtime-loaded Xevious pointers + loader. */

#include <Arduino.h>
#include "pico_romfs.h"   /* ROM zips: SD card or the flash ROM drive */
#include <stdint.h>

extern "C" {
    unsigned char  *xevious_rom_cpu1 = nullptr, *xevious_rom_cpu2 = nullptr, *xevious_rom_cpu3 = nullptr;
    unsigned char  *xevious_planetmap = nullptr;
    unsigned char  (*xevious_fgtiles)[8]  = nullptr;
    unsigned short (*xevious_bgtiles)[8]  = nullptr;
    unsigned long  (*xevious_sprites)[32] = nullptr;
    unsigned short (*xevious_cmap_fg)[2]  = nullptr;
    unsigned short (*xevious_cmap_bg)[4]  = nullptr;
    unsigned short (*xevious_cmap_spr)[8] = nullptr;
    signed char    (*xevious_wavetable)[32] = nullptr;
}

extern "C" unsigned char  *load_xevious_rom_from_zip      (const uint8_t *zip, size_t sz, int cpu);
extern "C" unsigned char  *load_xevious_planetmap_from_zip(const uint8_t *zip, size_t sz);
extern "C" unsigned char  *load_xevious_fgtiles_from_zip  (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_xevious_bgtiles_from_zip  (const uint8_t *zip, size_t sz);
extern "C" unsigned long  *load_xevious_sprites_from_zip  (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_xevious_colors_from_zip   (const uint8_t *zip, size_t sz);
extern "C" signed char    *load_xevious_wavetable_from_zip(const uint8_t *zip, size_t sz);

extern "C" bool runtime_load_xevious(void) {
    const char *path = "/roms/xevious.zip";
    uint8_t *zip = nullptr; size_t zip_sz = 0;
    if (!romfs_slurp(path, &zip, &zip_sz)) { Serial.println("[xevious] can't read xevious.zip"); return false; }

    xevious_rom_cpu1  = load_xevious_rom_from_zip(zip, zip_sz, 0);
    xevious_rom_cpu2  = load_xevious_rom_from_zip(zip, zip_sz, 1);
    xevious_rom_cpu3  = load_xevious_rom_from_zip(zip, zip_sz, 2);
    xevious_planetmap = load_xevious_planetmap_from_zip(zip, zip_sz);
    xevious_fgtiles   = (unsigned char (*)[8])load_xevious_fgtiles_from_zip(zip, zip_sz);
    xevious_bgtiles   = (unsigned short (*)[8])load_xevious_bgtiles_from_zip(zip, zip_sz);
    xevious_sprites   = (unsigned long (*)[32])load_xevious_sprites_from_zip(zip, zip_sz);
    unsigned short *col = load_xevious_colors_from_zip(zip, zip_sz);
    xevious_wavetable = (signed char (*)[32])load_xevious_wavetable_from_zip(zip, zip_sz);
    free(zip);
    if (col) {
        xevious_cmap_fg  = (unsigned short (*)[2])col;
        xevious_cmap_bg  = (unsigned short (*)[4])(col + 128);
        xevious_cmap_spr = (unsigned short (*)[8])(col + 640);
    }

    bool ok = xevious_rom_cpu1 && xevious_rom_cpu2 && xevious_rom_cpu3 && xevious_planetmap &&
              xevious_fgtiles && xevious_bgtiles && xevious_sprites && col && xevious_wavetable;
    Serial.printf("[xevious] runtime load %s\n", ok ? "OK" : "FAILED");
    return ok;
}
