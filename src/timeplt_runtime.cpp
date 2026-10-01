/* timeplt_runtime.cpp — runtime-loaded Time Pilot pointers + loader. */

#include <Arduino.h>
#include "pico_romfs.h"   /* ROM zips: SD card or the flash ROM drive */
#include <stdint.h>

extern "C" {
    unsigned char  *timeplt_rom_cpu1 = nullptr;
    unsigned char  *timeplt_rom_cpu2 = nullptr;
    unsigned short (*timeplt_tiles)[8]         = nullptr;
    unsigned long  (*timeplt_sprites)[16]      = nullptr;
    unsigned short (*timeplt_char_colors)[4]   = nullptr;
    unsigned short (*timeplt_sprite_colors)[4] = nullptr;
}

extern "C" unsigned char  *load_timeplt_rom_cpu1_from_zip(const uint8_t *zip, size_t sz);
extern "C" unsigned char  *load_timeplt_rom_cpu2_from_zip(const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_timeplt_tiles_from_zip   (const uint8_t *zip, size_t sz);
extern "C" unsigned long  *load_timeplt_sprites_from_zip (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_timeplt_colors_from_zip  (const uint8_t *zip, size_t sz);

extern "C" bool runtime_load_timeplt(void) {
    const char *path = "/roms/timeplt.zip";
    uint8_t *zip = nullptr; size_t zip_sz = 0;
    if (!romfs_slurp(path, &zip, &zip_sz)) { Serial.println("[timeplt] can't read timeplt.zip"); return false; }

    timeplt_rom_cpu1 = load_timeplt_rom_cpu1_from_zip(zip, zip_sz);
    timeplt_rom_cpu2 = load_timeplt_rom_cpu2_from_zip(zip, zip_sz);
    timeplt_tiles    = (unsigned short (*)[8])load_timeplt_tiles_from_zip(zip, zip_sz);
    timeplt_sprites  = (unsigned long (*)[16])load_timeplt_sprites_from_zip(zip, zip_sz);
    unsigned short *col = load_timeplt_colors_from_zip(zip, zip_sz);
    free(zip);
    if (col) {
        timeplt_char_colors   = (unsigned short (*)[4])col;
        timeplt_sprite_colors = (unsigned short (*)[4])(col + 128);
    }

    bool ok = timeplt_rom_cpu1 && timeplt_rom_cpu2 && timeplt_tiles && timeplt_sprites && col;
    Serial.printf("[timeplt] runtime load %s\n", ok ? "OK" : "FAILED");
    return ok;
}

