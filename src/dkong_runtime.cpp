/* dkong_runtime.cpp — runtime-loaded DKong pointers + loader. */

#include <Arduino.h>
#include <SD.h>
#include <stdint.h>

extern "C" {
    unsigned char  *dkong_rom_cpu1 = nullptr;
    unsigned char  *dkong_rom_cpu2 = nullptr;   /* i8048 audio ROM */
    unsigned short (*v_5h_b_bin)[8]              = nullptr;  /* tilemap, name auto-derived */
    unsigned long  (*dkong_sprites)[128][16]     = nullptr;
    unsigned short (*dkong_colormap)[256][4]     = nullptr;  /* [4 screens][256][4] */
    unsigned short (*dkong_colormap_sprite)[16][4] = nullptr;  /* [4 screens][16][4] */
}

extern "C" unsigned char  *load_dkong_rom_cpu1_from_zip     (const uint8_t*, size_t);
extern "C" unsigned char  *load_dkong_rom_cpu2_from_zip     (const uint8_t*, size_t);
extern "C" unsigned short *load_dkong_tilemap_from_zip      (const uint8_t*, size_t);
extern "C" unsigned long  *load_dkong_spritemap_from_zip    (const uint8_t*, size_t);
extern "C" unsigned short *load_dkong_cmap_from_zip         (const uint8_t*, size_t);
extern "C" unsigned short *load_dkong_cmap_sprite_from_zip  (const uint8_t*, size_t);

static bool sd_slurp(const char *path, uint8_t **out, size_t *out_sz) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    size_t sz = f.size();
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { f.close(); return false; }
    size_t got = f.read(buf, sz);
    f.close();
    if (got != sz) { free(buf); return false; }
    *out = buf; *out_sz = sz;
    return true;
}

extern "C" bool runtime_load_dkong(void) {
    const char *path = "/roms/dkong.zip";
    if (!SD.exists(path)) { Serial.println("[dkong] dkong.zip not on SD"); return false; }
    uint8_t *zip = nullptr; size_t zip_sz = 0;
    if (!sd_slurp(path, &zip, &zip_sz)) { Serial.println("[dkong] slurp failed"); return false; }
    Serial.printf("[dkong] loaded %s (%u bytes) — decoding...\n", path, (unsigned)zip_sz);

    dkong_rom_cpu1        = load_dkong_rom_cpu1_from_zip(zip, zip_sz);
    dkong_rom_cpu2        = load_dkong_rom_cpu2_from_zip(zip, zip_sz);
    v_5h_b_bin            = (unsigned short (*)[8])load_dkong_tilemap_from_zip(zip, zip_sz);
    dkong_sprites         = (unsigned long  (*)[128][16])load_dkong_spritemap_from_zip(zip, zip_sz);
    dkong_colormap        = (unsigned short (*)[256][4])load_dkong_cmap_from_zip(zip, zip_sz);
    /* IMPORTANT: cmap_sprite loader shares the palette built by cmap_from_zip,
     * so it MUST run AFTER the cmap loader. */
    dkong_colormap_sprite = (unsigned short (*)[16][4])load_dkong_cmap_sprite_from_zip(zip, zip_sz);
    free(zip);

    bool ok = dkong_rom_cpu1 && dkong_rom_cpu2
           && v_5h_b_bin && dkong_sprites
           && dkong_colormap && dkong_colormap_sprite;
    Serial.printf("[dkong] runtime load %s\n", ok ? "OK" : "FAILED");
    return ok;
}
