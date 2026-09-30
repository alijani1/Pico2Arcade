/* scobra_runtime.cpp — runtime-loaded Super Cobra pointers + loader. */

#include <Arduino.h>
#include "pico_romfs.h"   /* ROM zips: SD card or the flash ROM drive */
#include <stdint.h>

extern "C" {
    unsigned char  *scobra_rom_cpu1 = nullptr;
    unsigned char  *scobra_rom_cpu2 = nullptr;
    unsigned short (*scobra_tilemap)[8]        = nullptr;
    unsigned long  (*scobra_sprites)[64][16]   = nullptr;
    unsigned short (*scobra_colormap)[4]       = nullptr;
}

extern "C" unsigned char  *load_scobra_rom_cpu1_from_zip  (const uint8_t *zip, size_t sz);
extern "C" unsigned char  *load_scobra_rom_cpu2_from_zip  (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_scobra_tilemap_from_zip   (const uint8_t *zip, size_t sz);
extern "C" unsigned long  *load_scobra_spritemap_from_zip (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_scobra_cmap_from_zip      (const uint8_t *zip, size_t sz);
extern "C" void scobra_stars_init(void);

extern "C" bool runtime_load_scobra(void) {
    const char *path = "/roms/scobra.zip";
    uint8_t *zip = nullptr; size_t zip_sz = 0;
    if (!romfs_slurp(path, &zip, &zip_sz)) { Serial.println("[scobra] can't read scobra.zip"); return false; }

    scobra_rom_cpu1 = load_scobra_rom_cpu1_from_zip(zip, zip_sz);
    scobra_rom_cpu2 = load_scobra_rom_cpu2_from_zip(zip, zip_sz);
    scobra_tilemap  = (unsigned short (*)[8])load_scobra_tilemap_from_zip(zip, zip_sz);
    scobra_sprites  = (unsigned long (*)[64][16])load_scobra_spritemap_from_zip(zip, zip_sz);
    scobra_colormap = (unsigned short (*)[4])load_scobra_cmap_from_zip(zip, zip_sz);
    free(zip);

    bool ok = scobra_rom_cpu1 && scobra_rom_cpu2 && scobra_tilemap && scobra_sprites && scobra_colormap;
    if (ok) scobra_stars_init();
    Serial.printf("[scobra] runtime load %s\n", ok ? "OK" : "FAILED");
    return ok;
}
