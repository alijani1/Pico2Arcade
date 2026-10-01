/* pooyan_runtime.cpp — runtime-loaded Pooyan pointers + loader. */

#include <Arduino.h>
#include "pico_romfs.h"   /* ROM zips: SD card or the flash ROM drive */
#include <stdint.h>

extern "C" {
    unsigned char  *pooyan_rom_cpu1 = nullptr;
    unsigned char  *pooyan_rom_cpu2 = nullptr;
    unsigned long  (*pooyan_tiles)[8]          = nullptr;
    unsigned char  (*pooyan_sprites)[16][16]   = nullptr;
    unsigned short (*pooyan_char_colors)[16]   = nullptr;
    unsigned short (*pooyan_sprite_colors)[16] = nullptr;
    unsigned short *pooyan_sprite_transmask    = nullptr;
}

extern "C" unsigned char  *load_pooyan_rom_cpu1_from_zip(const uint8_t *zip, size_t sz);
extern "C" unsigned char  *load_pooyan_rom_cpu2_from_zip(const uint8_t *zip, size_t sz);
extern "C" unsigned long  *load_pooyan_tiles_from_zip   (const uint8_t *zip, size_t sz);
extern "C" unsigned char  *load_pooyan_sprites_from_zip (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_pooyan_colors_from_zip  (const uint8_t *zip, size_t sz);

extern "C" bool runtime_load_pooyan(void) {
    const char *path = "/roms/pooyan.zip";
    uint8_t *zip = nullptr; size_t zip_sz = 0;
    if (!romfs_slurp(path, &zip, &zip_sz)) { Serial.println("[pooyan] can't read pooyan.zip"); return false; }

    pooyan_rom_cpu1 = load_pooyan_rom_cpu1_from_zip(zip, zip_sz);
    pooyan_rom_cpu2 = load_pooyan_rom_cpu2_from_zip(zip, zip_sz);
    pooyan_tiles    = (unsigned long (*)[8])load_pooyan_tiles_from_zip(zip, zip_sz);
    pooyan_sprites  = (unsigned char (*)[16][16])load_pooyan_sprites_from_zip(zip, zip_sz);
    unsigned short *col = load_pooyan_colors_from_zip(zip, zip_sz);
    free(zip);
    if (col) {
        pooyan_char_colors      = (unsigned short (*)[16])col;
        pooyan_sprite_colors    = (unsigned short (*)[16])(col + 256);
        pooyan_sprite_transmask = col + 512;
    }

    bool ok = pooyan_rom_cpu1 && pooyan_rom_cpu2 && pooyan_tiles && pooyan_sprites && col;
    Serial.printf("[pooyan] runtime load %s\n", ok ? "OK" : "FAILED");
    return ok;
}
