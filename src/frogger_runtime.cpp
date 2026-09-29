/* frogger_runtime.cpp — runtime-loaded Frogger pointers + loader. */

#include <Arduino.h>
#include "pico_romfs.h"   /* ROM zips: SD card or the Quad ROM drive */
#include <stdint.h>

extern "C" {
    unsigned char  *frogger_rom_cpu1 = nullptr;
    unsigned char  *frogger_rom_cpu2 = nullptr;
    unsigned short (*frogger_606)[8]       = nullptr;  /* tilemap, name from filename */
    unsigned long  (*frogger_sprites)[64][16] = nullptr;
    unsigned short (*frogger_colormap)[4]  = nullptr;  /* [8][4] layout */
}

/* From mame_rom_loader.cpp — file-scope helper we use to dump entries
 * when a filename mismatch is suspected. */
static inline uint16_t _rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t _rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void dump_zip_entries(const uint8_t *zip, size_t sz) {
    const uint8_t *p = zip, *end = zip + sz;
    Serial.println("[frogger] zip contents:");
    while (p + 30 <= end && _rd32(p) == 0x04034b50u) {
        uint16_t nlen = _rd16(p + 26), elen = _rd16(p + 28);
        uint32_t usize = _rd32(p + 22);
        char nbuf[48]; int nl = nlen < 47 ? nlen : 47;
        memcpy(nbuf, p + 30, nl); nbuf[nl] = 0;
        Serial.printf("[frogger]   %-20s uncompressed=%lu\n", nbuf, (unsigned long)usize);
        p = p + 30 + nlen + elen + _rd32(p + 18);
    }
}
extern "C" unsigned char  *load_frogger_rom_cpu1_from_zip  (const uint8_t *zip, size_t sz);
extern "C" unsigned char  *load_frogger_rom_cpu2_from_zip  (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_frogger_tilemap_from_zip   (const uint8_t *zip, size_t sz);
extern "C" unsigned long  *load_frogger_spritemap_from_zip (const uint8_t *zip, size_t sz);
extern "C" unsigned short *load_frogger_cmap_from_zip      (const uint8_t *zip, size_t sz);

static bool sd_slurp(const char *path, uint8_t **out, size_t *out_sz) {
    return romfs_slurp(path, out, out_sz);
}

extern "C" bool runtime_load_frogger(void) {
    const char *path = "/roms/frogger.zip";
    if (!romfs_exists(path)) { Serial.println("[frogger] frogger.zip not on SD"); return false; }

    uint8_t *zip = nullptr; size_t zip_sz = 0;
    if (!sd_slurp(path, &zip, &zip_sz)) { Serial.println("[frogger] slurp failed"); return false; }
    Serial.printf("[frogger] loaded %s (%u bytes) — decoding...\n", path, (unsigned)zip_sz);
    dump_zip_entries(zip, zip_sz);

    frogger_rom_cpu1 = load_frogger_rom_cpu1_from_zip(zip, zip_sz);
    frogger_rom_cpu2 = load_frogger_rom_cpu2_from_zip(zip, zip_sz);
    frogger_606      = (unsigned short (*)[8])load_frogger_tilemap_from_zip(zip, zip_sz);
    frogger_sprites  = (unsigned long  (*)[64][16])load_frogger_spritemap_from_zip(zip, zip_sz);
    frogger_colormap = (unsigned short (*)[4])load_frogger_cmap_from_zip(zip, zip_sz);
    free(zip);

    bool ok = frogger_rom_cpu1 && frogger_rom_cpu2
           && frogger_606 && frogger_sprites && frogger_colormap;
    Serial.printf("[frogger] runtime load %s\n", ok ? "OK" : "FAILED");
    return ok;
}
