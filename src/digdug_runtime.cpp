/* digdug_runtime.cpp — runtime-loaded Digdug pointers + loader. */

#include <Arduino.h>
#include "pico_romfs.h"   /* ROM zips: SD card or the Quad ROM drive */
#include <stdint.h>

extern "C" {
    unsigned char  *digdug_rom_cpu1 = nullptr;
    unsigned char  *digdug_rom_cpu2 = nullptr;
    unsigned char  *digdug_rom_cpu3 = nullptr;
    unsigned char  *digdug_playfield = nullptr;
    unsigned short (*dd1_9)[8]                    = nullptr;
    unsigned short (*dd1_11)[8]                   = nullptr;
    unsigned long  (*digdug_sprites)[256][16]     = nullptr;
    unsigned short (*digdug_colormap_tiles)[4]    = nullptr;
    unsigned short (*digdug_colormap_sprites)[4]  = nullptr;
    unsigned short (*digdug_colormaps)[4]         = nullptr;
    signed char    (*digdug_wavetable)[32]        = nullptr;
}

extern "C" unsigned char  *load_digdug_rom_cpu1_from_zip     (const uint8_t *zip, size_t sz);
extern "C" unsigned char  *load_digdug_rom_cpu2_from_zip     (const uint8_t *zip, size_t sz);
extern "C" unsigned char  *load_digdug_rom_cpu3_from_zip     (const uint8_t *zip, size_t sz);
extern "C" unsigned char  *load_digdug_playfield_from_zip    (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_digdug_tilemap_from_zip      (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_digdug_pftiles_from_zip      (const uint8_t *zip, size_t sz);
extern "C" unsigned long  *load_digdug_spritemap_from_zip    (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_digdug_cmap_tiles_from_zip   (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_digdug_cmap_sprites_from_zip (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_digdug_cmap_from_zip         (const uint8_t *zip, size_t sz);
extern "C" signed char    *load_digdug_wavetable_from_zip    (const uint8_t *zip, size_t sz);

static bool sd_slurp(const char *path, uint8_t **out, size_t *out_sz) {
    return romfs_slurp(path, out, out_sz);
}

extern "C" bool runtime_load_digdug(void) {
    const char *path = "/roms/digdug.zip";
    if (!romfs_exists(path)) { Serial.println("[digdug] digdug.zip not on SD"); return false; }

    uint8_t *zip = nullptr; size_t zip_sz = 0;
    if (!sd_slurp(path, &zip, &zip_sz)) { Serial.println("[digdug] slurp failed"); return false; }
    Serial.printf("[digdug] loaded %s (%u bytes) — decoding...\n", path, (unsigned)zip_sz);

    /* Dump ZIP entries so any name mismatch (dd1.11 vs digdug.11 etc.) is
     * obvious in the boot log. */
    {
        const uint8_t *p = zip, *end = zip + zip_sz;
        auto rd16 = [](const uint8_t *q){ return (uint16_t)(q[0] | (q[1] << 8)); };
        auto rd32 = [](const uint8_t *q){
            return (uint32_t)q[0] | ((uint32_t)q[1]<<8) | ((uint32_t)q[2]<<16) | ((uint32_t)q[3]<<24);
        };
        Serial.println("[digdug] zip contents:");
        while (p + 30 <= end && rd32(p) == 0x04034b50u) {
            uint16_t nlen = rd16(p + 26), elen = rd16(p + 28);
            uint32_t usize = rd32(p + 22);
            char nbuf[48]; int nl = nlen < 47 ? nlen : 47;
            memcpy(nbuf, p + 30, nl); nbuf[nl] = 0;
            Serial.printf("[digdug]   %-20s uncompressed=%lu\n", nbuf, (unsigned long)usize);
            p = p + 30 + nlen + elen + rd32(p + 18);
        }
    }

    digdug_rom_cpu1         = load_digdug_rom_cpu1_from_zip(zip, zip_sz);
    digdug_rom_cpu2         = load_digdug_rom_cpu2_from_zip(zip, zip_sz);
    digdug_rom_cpu3         = load_digdug_rom_cpu3_from_zip(zip, zip_sz);
    digdug_playfield        = load_digdug_playfield_from_zip(zip, zip_sz);
    dd1_9                   = (unsigned short (*)[8])load_digdug_tilemap_from_zip(zip, zip_sz);
    dd1_11                  = (unsigned short (*)[8])load_digdug_pftiles_from_zip(zip, zip_sz);
    digdug_sprites          = (unsigned long  (*)[256][16])load_digdug_spritemap_from_zip(zip, zip_sz);
    digdug_colormap_tiles   = (unsigned short (*)[4])load_digdug_cmap_tiles_from_zip(zip, zip_sz);
    digdug_colormap_sprites = (unsigned short (*)[4])load_digdug_cmap_sprites_from_zip(zip, zip_sz);
    digdug_colormaps        = (unsigned short (*)[4])load_digdug_cmap_from_zip(zip, zip_sz);
    digdug_wavetable        = (signed char    (*)[32])load_digdug_wavetable_from_zip(zip, zip_sz);
    free(zip);

    bool ok = digdug_rom_cpu1 && digdug_rom_cpu2 && digdug_rom_cpu3
           && digdug_playfield && dd1_9 && dd1_11 && digdug_sprites
           && digdug_colormap_tiles && digdug_colormap_sprites
           && digdug_colormaps && digdug_wavetable;
    Serial.printf("[digdug] runtime load %s\n", ok ? "OK" : "FAILED");
    return ok;
}
