/* mame_rom_loader.cpp — read a MAME ROM ZIP off SD, unpack entries in RAM.
 *
 * Milestone 2 of the runtime ROM-loading plan: prove we can open a
 * .zip file on SD, enumerate its entries, and inflate each entry into
 * a malloc'd buffer. Once this is working we plug it into the game-
 * specific converters (romconv/cmapconv/tileconv/spriteconv/audioconv)
 * and the pre-baked *_rom.h etc. headers go away.
 *
 * ZIP support: STORED (method 0) and DEFLATE (method 8). Encrypted or
 * unusually large (ZIP64) entries are rejected — MAME sets never use
 * those.
 */

#include <Arduino.h>
#include "rom_scratch.h"
#include "pico_romfs.h"   /* ROM zips: SD card or the Quad ROM drive */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include "uzlib.h"
}
#include "pico_romcheck.h"   /* romcheck_crcs_for: find renamed ROM files by CRC */

/* Pre-baked headers no longer #included — the game now reads via
 * runtime pointers populated by runtime_load_pacman(). */

/* Little-endian byte reads out of a byte buffer. */
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Slurp an entire file from SD into a malloc'd buffer. Caller frees. */
static bool sd_slurp(const char *path, uint8_t **out_buf, size_t *out_size) {
    return romfs_slurp(path, out_buf, out_size);
}

/* Extract one entry by name into `dst`. Returns true on success.
 * Handles STORED (memcpy) and DEFLATE (uzlib). If no entry has that name,
 * the entry whose CRC32 is one the ROM check expects for the name is used
 * instead: MAME renames ROM files between versions (e.g. dkongjr's dkj.5b
 * vs djr1-c_5b_f-2.5b), the contents stay the same. */
static bool zip_extract_one(const uint8_t *zip, size_t zip_size,
                            const char *want_name, const uint32_t *want_crc, int ncrc,
                            uint8_t *dst, size_t dst_capacity,
                            size_t *out_size) {
    const uint8_t *p = zip;
    const uint8_t *end = zip + zip_size;
    while (p + 30 <= end) {
        if (rd32(p) != 0x04034b50u) return false;
        uint16_t method  = rd16(p + 8);
        uint32_t crc     = rd32(p + 14);
        uint32_t csize   = rd32(p + 18);
        uint32_t usize   = rd32(p + 22);
        uint16_t nlen    = rd16(p + 26);
        uint16_t elen    = rd16(p + 28);
        const char *name = (const char *)(p + 30);
        const uint8_t *data = p + 30 + nlen + elen;
        if (data + csize > end) return false;

        char nbuf[48];
        int nl = nlen < (int)sizeof(nbuf) - 1 ? nlen : (int)sizeof(nbuf) - 1;
        memcpy(nbuf, name, nl); nbuf[nl] = 0;

        bool match = want_name ? (strcmp(nbuf, want_name) == 0) : false;
        for (int i = 0; !match && i < ncrc; i++) match = (crc == want_crc[i]);
        if (match) {
            if (usize > dst_capacity) return false;
            if (method == 0) {
                if (csize != usize) return false;
                memcpy(dst, data, csize);
                if (out_size) *out_size = usize;
                return true;
            }
            if (method == 8) {
                TINF_DATA d;
                memset(&d, 0, sizeof(d));
                d.source = data;
                d.source_limit = data + csize;
                d.dest = dst;
                d.dest_start = dst;
                d.dest_limit = dst + usize;
                uzlib_uncompress_init(&d, NULL, 0);
                int rc = uzlib_uncompress(&d);
                if (rc != 0 && rc != TINF_DONE) {
                    Serial.printf("[loader] uzlib rc=%d for %s\n", rc, nbuf);
                    return false;
                }
                if (out_size) *out_size = usize;
                return true;
            }
            Serial.printf("[loader] unsupported method %u for %s\n", method, nbuf);
            return false;
        }
        p = data + csize;
    }
    return false;
}

static bool zip_extract(const uint8_t *zip, size_t zip_size,
                        const char *want_name,
                        uint8_t *dst, size_t dst_capacity,
                        size_t *out_size) {
    if (zip_extract_one(zip, zip_size, want_name, nullptr, 0, dst, dst_capacity, out_size))
        return true;
    uint32_t crcs[4];
    int n = romcheck_crcs_for(want_name, crcs, 4);
    if (n && zip_extract_one(zip, zip_size, nullptr, crcs, n, dst, dst_capacity, out_size)) {
        Serial.printf("[loader] %s: found by CRC under another name\n", want_name);
        return true;
    }
    return false;
}

/* Public wrapper for callers outside this TU (e.g. mspacman_runtime.cpp). */
extern "C" bool zip_extract_public(const uint8_t *zip, size_t zip_size,
                                   const char *want_name,
                                   uint8_t *dst, size_t dst_capacity,
                                   size_t *out_size) {
    return zip_extract(zip, zip_size, want_name, dst, dst_capacity, out_size);
}

/* Iterate ZIP local file headers, printing each entry name + sizes. */
static void zip_dump(const uint8_t *zip, size_t zip_size) {
    const uint8_t *p = zip;
    const uint8_t *end = zip + zip_size;
    int n = 0;
    while (p + 30 <= end) {
        if (rd32(p) != 0x04034b50u) break;    /* not a local file header */
        uint16_t flags   = rd16(p + 6);
        uint16_t method  = rd16(p + 8);
        uint32_t csize   = rd32(p + 18);
        uint32_t usize   = rd32(p + 22);
        uint16_t nlen    = rd16(p + 26);
        uint16_t elen    = rd16(p + 28);
        const uint8_t *name = p + 30;
        const uint8_t *data = name + nlen + elen;
        if (data > end) { Serial.println("[loader] truncated header"); break; }

        char nbuf[48];
        int nl = nlen < (int)sizeof(nbuf) - 1 ? nlen : (int)sizeof(nbuf) - 1;
        memcpy(nbuf, name, nl); nbuf[nl] = 0;

        const char *methodStr = (method == 0) ? "STORED" :
                                (method == 8) ? "DEFLATE" : "OTHER";
        Serial.printf("[loader]   %-24s  method=%s  compressed=%lu  uncompressed=%lu\n",
                      nbuf, methodStr,
                      (unsigned long)csize, (unsigned long)usize);

        /* If bit 3 of flags is set, sizes live in a data descriptor
         * after the compressed data — we don't handle that yet, so
         * skip further parsing. MAME sets don't use it. */
        if (flags & 0x08) {
            Serial.println("[loader] data descriptor present — stopping (unsupported)");
            break;
        }

        p = data + csize;
        n++;
        if (n > 40) { Serial.println("[loader] entry cap reached"); break; }
    }
    Serial.printf("[loader] %d entries listed\n", n);
}

/* Public entry — call from setup() after pico_sd_init(). Tries the
 * Namco Puck Man set first (16 files, includes PROMs), falls back to
 * the Midway Pac-Man set. */
/* Build the Pac-Man colormap: `unsigned short pacman_colormap[64][4]`.
 * Combines the 32-byte palette PROM (pm1-1.7f, bbgggrrr → RGB565) with
 * the 256-byte lookup PROM (pm1-4.4a, 64 groups of 4 palette indices).
 * Output is 256 bytes (128 uint16 = 64*4), byte-swapped for the display. */
extern "C" unsigned short *load_pacman_cmap_from_zip(const uint8_t *zip, size_t zip_size) {
    uint8_t palette_prom[32];
    uint8_t colormap_prom[256];
    size_t got = 0;
    if (!zip_extract(zip, zip_size, "pm1-1.7f", palette_prom, 32, &got) || got != 32) {
        Serial.println("[loader] pm1-1.7f extract failed"); return nullptr;
    }
    if (!zip_extract(zip, zip_size, "pm1-4.4a", colormap_prom, 256, &got) || got != 256) {
        Serial.println("[loader] pm1-4.4a extract failed"); return nullptr;
    }

    /* Build 32 RGB565 palette entries (bbgggrrr format on Pac-Man). */
    uint16_t palette[32];
    for (int i = 0; i < 32; i++) {
        uint8_t c = palette_prom[i];
        int b = 31 * ((c >> 6) & 0x3) / 3;
        int g = 63 * ((c >> 3) & 0x7) / 7;
        int r = 31 * ((c >> 0) & 0x7) / 7;
        uint16_t rgb = (uint16_t)((r << 11) | (g << 5) | b);
        /* Byte-swap for the display (matches Python's `rgbs = ...` swap). */
        palette[i] = (uint16_t)(((rgb & 0xff00) >> 8) | ((rgb & 0xff) << 8));
    }

    /* 64 groups of 4 palette-index bytes → 64*4 RGB565 lookup. */
    unsigned short *out = (unsigned short *)malloc(64 * 4 * sizeof(unsigned short));
    if (!out) return nullptr;
    for (int i = 0; i < 64; i++) {
        for (int j = 0; j < 4; j++) {
            uint8_t idx = (uint8_t)(colormap_prom[i * 4 + j] & 0x0F);
            out[i * 4 + j] = palette[idx];   /* uses palette[0:16] (offset=0) */
        }
    }
    return out;
}

/* Build Pac-Man wavetable: `signed char pacman_wavetable[16][32]`.
 * Concatenates the two 256-byte wavetable PROMs (pm1-3.1m first, then
 * pm1-2.3m — order from conv.sh: 82s126.1m maps to pm1-3.1m). Each byte
 * is a 4-bit sample; shift by -7 to center on zero (0..15 → -7..+8). */
extern "C" signed char *load_pacman_wavetable_from_zip(const uint8_t *zip, size_t zip_size) {
    /* conv.sh: audioconv.py pacman_wavetable 82s126.1m 82s126.3m
     * The 82s126.1m file → pm1-3.1m; 82s126.3m → pm1-2.3m. */
    static const char *prom_names[2] = { "pm1-3.1m", "pm1-2.3m" };
    signed char *out = (signed char *)malloc(16 * 32);
    if (!out) return nullptr;
    for (int p = 0; p < 2; p++) {
        uint8_t raw[256];
        size_t got = 0;
        if (!zip_extract(zip, zip_size, prom_names[p], raw, 256, &got) || got != 256) {
            Serial.printf("[loader] %s extract failed\n", prom_names[p]);
            free(out);
            return nullptr;
        }
        for (int i = 0; i < 256; i++)
            out[p * 256 + i] = (signed char)((int)raw[i] - 7);
    }
    return out;
}

/* Decode one 8x8 Pac-Man/Galaga tile from 16 bytes of planar data into
 * 8 uint16 (each holds 8 pixels of 2 bits). Port of Python parse_chr +
 * dump_chr for pacman format. */
static void decode_tile_pac(const uint8_t *data, uint16_t out_row[8]) {
    for (int y = 0; y < 8; y++) {
        uint16_t val = 0;
        for (int x = 0; x < 8; x++) {
            uint8_t byte = data[15 - x - 2 * (y & 4)];
            int c0 = (byte & (0x08 >> (y & 3))) ? 1 : 0;
            int c1 = (byte & (0x80 >> (y & 3))) ? 2 : 0;
            int pix = c0 + c1;
            val = (uint16_t)((val >> 2) | (pix << 14));
        }
        out_row[y] = val;
    }
}

/* Build Pac-Man tilemap: `unsigned short pacman_5e[256][8]` = 4 KB.
 * Combines pm1_chg1.5e + pm1_chg2.5h into a 4 KB planar buffer, then
 * decodes 256 x 8x8 tiles. */
extern "C" unsigned short *load_pacman_tilemap_from_zip(const uint8_t *zip, size_t zip_size) {
    RomScratch raw(4096); if (!raw) return nullptr;
    size_t got = 0;
    if (!zip_extract(zip, zip_size, "pm1_chg1.5e", raw + 0,    2048, &got) || got != 2048) return nullptr;
    if (!zip_extract(zip, zip_size, "pm1_chg2.5h", raw + 2048, 2048, &got) || got != 2048) return nullptr;
    unsigned short *out = (unsigned short *)malloc(256 * 8 * sizeof(unsigned short));
    if (!out) return nullptr;
    for (int c = 0; c < 256; c++) decode_tile_pac(&raw[16 * c], &out[c * 8]);
    return out;
}

/* Decode one 16x16 Pac-Man sprite from 64 bytes. Returns 16x16 pixel
 * grid (2 bits per pixel). Pac-Man format: top 4 rows are stored in
 * the LAST bytes, so rows get rotated at the end. */
static void decode_sprite_pac(const uint8_t *data, uint8_t pix[16][16]) {
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int idx = ((y & 8) << 1) + (((x & 8) ^ 8) << 2) + (7 - (x & 7)) + 2 * (y & 4);
            uint8_t b = data[idx];
            int c0 = (b & (0x08 >> (y & 3))) ? 1 : 0;
            int c1 = (b & (0x80 >> (y & 3))) ? 2 : 0;
            pix[y][x] = (uint8_t)(c0 + c1);
        }
    }
    /* pacman_fmt: shift so top 4 rows come from bottom 4 (see python). */
    uint8_t rotated[16][16];
    for (int i = 0; i < 12; i++) memcpy(rotated[i],  pix[i + 4], 16);
    for (int i = 0; i < 4;  i++) memcpy(rotated[12 + i], pix[i], 16);
    memcpy(pix, rotated, 16 * 16);
}

/* Pack a 16-row sprite into 16 uint32 with optional flip. Port of
 * Python dump_sprite. Each uint32 holds 16 pixels × 2 bits = 32 bits. */
static void pack_sprite_flip(const uint8_t pix[16][16], bool flip_x, bool flip_y,
                             uint32_t out[16]) {
    for (int oy = 0; oy < 16; oy++) {
        int y = flip_y ? (15 - oy) : oy;
        uint32_t val = 0;
        for (int ox = 0; ox < 16; ox++) {
            int x = ox;   /* both variants of flip_x are handled explicitly */
            if (!flip_x) val = (val >> 2) | ((uint32_t)pix[y][x] << 30);
            else         val = (val << 2) | pix[y][x];
        }
        out[oy] = val;
    }
}

/* Build Pac-Man spritemap: `unsigned long pacman_sprites[4][64][16]` = 16 KB.
 * Order of the outer 4 variants matches python: no-flip, y-flip,
 * x-flip, xy-flip. */
extern "C" unsigned long *load_pacman_spritemap_from_zip(const uint8_t *zip, size_t zip_size) {
    RomScratch raw(4096); if (!raw) return nullptr;
    size_t got = 0;
    if (!zip_extract(zip, zip_size, "pm1_chg3.5f", raw + 0,    2048, &got) || got != 2048) return nullptr;
    if (!zip_extract(zip, zip_size, "pm1_chg4.5j", raw + 2048, 2048, &got) || got != 2048) return nullptr;
    unsigned long *out = (unsigned long *)malloc(4 * 64 * 16 * sizeof(unsigned long));
    if (!out) return nullptr;
    const bool flip_x[4] = { false, false, true, true };
    const bool flip_y[4] = { false, true,  false, true };
    for (int v = 0; v < 4; v++) {
        for (int s = 0; s < 64; s++) {
            uint8_t pix[16][16];
            decode_sprite_pac(&raw[64 * s], pix);
            uint32_t rows[16];
            pack_sprite_flip(pix, flip_x[v], flip_y[v], rows);
            for (int r = 0; r < 16; r++)
                out[v * 64 * 16 + s * 16 + r] = (unsigned long)rows[r];
        }
    }
    return out;
}

/* Load the Pac-Man code ROM from a puckman.zip in-RAM copy.
 * Concatenates pm1_prg1..8 in order into a fresh 16 KB buffer.
 * Caller frees. Returns nullptr on failure. */
extern "C" unsigned char *load_pacman_rom_from_zip(const uint8_t *zip, size_t zip_size) {
    static const char *prg_names[8] = {
        "pm1_prg1.6e", "pm1_prg2.6k", "pm1_prg3.6f", "pm1_prg4.6m",
        "pm1_prg5.6h", "pm1_prg6.6n", "pm1_prg7.6j", "pm1_prg8.6p",
    };
    unsigned char *out = (unsigned char *)malloc(16384);
    if (!out) return nullptr;
    for (int i = 0; i < 8; i++) {
        size_t got = 0;
        if (!zip_extract(zip, zip_size, prg_names[i],
                         out + (size_t)i * 2048, 2048, &got)
            || got != 2048) {
            Serial.printf("[loader] pacman_rom: extract %s failed\n", prg_names[i]);
            free(out);
            return nullptr;
        }
    }
    return out;
}

/* pico_test_load_pacman removed — verification proved byte-perfect;
 * the real runtime load happens in pacman_runtime.cpp. */

/* ============================== GALAGA ============================== */

/* One patch entry as applied by Python romconv.py -p. Only Galaga's cpu1
 * uses these — 5 bytes to skip the RAM/ROM tests during boot. */
struct RomPatch { uint32_t offset; uint8_t expect; uint8_t replace; };
static const RomPatch galaga_cpu1_patches[] = {
    { 0x3382, 0x06, 0xc3 },   /* 06 → JP 0x3435 */
    { 0x3383, 0x0a, 0x35 },
    { 0x3384, 0xd9, 0x34 },
    { 0x348a, 0x1e, 0x01 },   /* 1 ramtest round instead of 30 */
    { 0x352b, 0xe5, 0xc9 },   /* RET — skip rom test */
};

/* Load Galaga CPU1 ROM (16 KB): concat 4 x 4K files + apply 5 patches. */
extern "C" unsigned char *load_galaga_rom_cpu1_from_zip(const uint8_t *zip, size_t sz) {
    static const char *names[4] = {
        "gg1_1b.3p", "gg1_2b.3m", "gg1_3.2m", "gg1_4b.2l",
    };
    unsigned char *out = (unsigned char *)malloc(16384);
    if (!out) return nullptr;
    for (int i = 0; i < 4; i++) {
        size_t got = 0;
        if (!zip_extract(zip, sz, names[i], out + (size_t)i * 4096, 4096, &got) || got != 4096) {
            Serial.printf("[loader] galaga cpu1 %s failed\n", names[i]);
            free(out); return nullptr;
        }
    }
    for (auto &p : galaga_cpu1_patches) {
        if (out[p.offset] != p.expect) {
            Serial.printf("[loader] galaga patch @0x%X expected %02X got %02X\n",
                          (unsigned)p.offset, p.expect, out[p.offset]);
        }
        out[p.offset] = p.replace;
    }
    return out;
}

/* Load Galaga CPU2 or CPU3 (4 KB each, single file). */
extern "C" unsigned char *load_galaga_rom_cpu2_from_zip(const uint8_t *zip, size_t sz) {
    unsigned char *out = (unsigned char *)malloc(4096);
    if (!out) return nullptr;
    size_t got = 0;
    if (!zip_extract(zip, sz, "gg1_5b.3f", out, 4096, &got) || got != 4096) { free(out); return nullptr; }
    return out;
}
extern "C" unsigned char *load_galaga_rom_cpu3_from_zip(const uint8_t *zip, size_t sz) {
    unsigned char *out = (unsigned char *)malloc(4096);
    if (!out) return nullptr;
    size_t got = 0;
    if (!zip_extract(zip, sz, "gg1_7b.2c", out, 4096, &got) || got != 4096) { free(out); return nullptr; }
    return out;
}

/* Galaga tilemap: same 8x8 2bpp format as Pac-Man, single 4 KB file. */
extern "C" unsigned short *load_galaga_tilemap_from_zip(const uint8_t *zip, size_t sz) {
    RomScratch raw(4096); if (!raw) return nullptr;
    size_t got = 0;
    if (!zip_extract(zip, sz, "gg1_9.4l", raw, 4096, &got) || got != 4096) return nullptr;
    unsigned short *out = (unsigned short *)malloc(256 * 8 * sizeof(unsigned short));
    if (!out) return nullptr;
    for (int c = 0; c < 256; c++) decode_tile_pac(&raw[16 * c], &out[c * 8]);
    return out;
}

/* Galaga spritemap: 128 sprites × 4 flip variants × 16 rows × 4 bytes = 32 KB.
 * Sprite format is "galaga" — same bit-plane layout as Pac-Man BUT no row
 * rotation at the end (that's a Pac-Man-only quirk). */
static void decode_sprite_galaga(const uint8_t *data, uint8_t pix[16][16]) {
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int idx = ((y & 8) << 1) + (((x & 8) ^ 8) << 2) + (7 - (x & 7)) + 2 * (y & 4);
            uint8_t b = data[idx];
            int c0 = (b & (0x08 >> (y & 3))) ? 1 : 0;
            int c1 = (b & (0x80 >> (y & 3))) ? 2 : 0;
            pix[y][x] = (uint8_t)(c0 + c1);
        }
    }
    /* NO row rotation (unlike Pac-Man). */
}

extern "C" unsigned long *load_galaga_spritemap_from_zip(const uint8_t *zip, size_t sz) {
    RomScratch raw(8192); if (!raw) return nullptr;   /* 2 x 4K sprite ROMs */
    size_t got = 0;
    if (!zip_extract(zip, sz, "gg1_11.4d", raw + 0,    4096, &got) || got != 4096) return nullptr;
    if (!zip_extract(zip, sz, "gg1_10.4f", raw + 4096, 4096, &got) || got != 4096) return nullptr;
    unsigned long *out = (unsigned long *)malloc(4 * 128 * 16 * sizeof(unsigned long));
    if (!out) return nullptr;
    const bool flip_x[4] = { false, false, true, true };
    const bool flip_y[4] = { false, true,  false, true };
    for (int v = 0; v < 4; v++) {
        for (int s = 0; s < 128; s++) {
            uint8_t pix[16][16];
            decode_sprite_galaga(&raw[64 * s], pix);
            uint32_t rows[16];
            pack_sprite_flip(pix, flip_x[v], flip_y[v], rows);
            for (int r = 0; r < 16; r++)
                out[v * 128 * 16 + s * 16 + r] = (unsigned long)rows[r];
        }
    }
    return out;
}

/* Galaga cmap: shared 32-byte palette PROM (prom-5.5n), sliced with
 * offset 0 for sprites, offset 16 for tiles. Then a 256-byte lookup PROM
 * (prom-3.1c for sprites, prom-4.2n for tiles) — 64 groups × 4 palette-
 * index bytes. Format shared with Pac-Man; only the offset differs. */
static unsigned short *build_galaga_cmap(const uint8_t *zip, size_t sz,
                                         const char *lookup_prom_name,
                                         int palette_offset) {
    uint8_t palette_prom[32];
    uint8_t lookup_prom[256];
    size_t got = 0;
    if (!zip_extract(zip, sz, "prom-5.5n", palette_prom, 32, &got) || got != 32) return nullptr;
    if (!zip_extract(zip, sz, lookup_prom_name, lookup_prom, 256, &got) || got != 256) return nullptr;

    uint16_t palette[32];
    for (int i = 0; i < 32; i++) {
        uint8_t c = palette_prom[i];
        int b = 31 * ((c >> 6) & 0x3) / 3;
        int g = 63 * ((c >> 3) & 0x7) / 7;
        int r = 31 * ((c >> 0) & 0x7) / 7;
        uint16_t rgb = (uint16_t)((r << 11) | (g << 5) | b);
        palette[i] = (uint16_t)(((rgb & 0xff00) >> 8) | ((rgb & 0xff) << 8));
    }

    unsigned short *out = (unsigned short *)malloc(64 * 4 * sizeof(unsigned short));
    if (!out) return nullptr;
    for (int i = 0; i < 64; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = palette[palette_offset + (lookup_prom[i * 4 + j] & 0x0F)];
    return out;
}
extern "C" unsigned short *load_galaga_cmap_sprites_from_zip(const uint8_t *zip, size_t sz) {
    return build_galaga_cmap(zip, sz, "prom-3.1c", 0);   /* sprites: offset 0 */
}
extern "C" unsigned short *load_galaga_cmap_tiles_from_zip(const uint8_t *zip, size_t sz) {
    return build_galaga_cmap(zip, sz, "prom-4.2n", 16);  /* tiles: offset 16 */
}

/* Galaga wavetable: single 256 B PROM (prom-1.1d) — 8 waveforms × 32 samples. */
extern "C" signed char *load_galaga_wavetable_from_zip(const uint8_t *zip, size_t sz) {
    uint8_t raw[256];
    size_t got = 0;
    if (!zip_extract(zip, sz, "prom-1.1d", raw, 256, &got) || got != 256) return nullptr;
    signed char *out = (signed char *)malloc(8 * 32);
    if (!out) return nullptr;
    for (int i = 0; i < 256; i++) out[i] = (signed char)((int)raw[i] - 7);
    return out;
}

/* ============================== FROGGER ============================== */

/* Load Frogger CPU1 (3 x 4K = 12K, padded to 16K). */
extern "C" unsigned char *load_frogger_rom_cpu1_from_zip(const uint8_t *zip, size_t sz) {
    static const char *names[3] = { "frogger.26", "frogger.27", "frsm3.7" };
    unsigned char *out = (unsigned char *)malloc(16384);
    if (!out) return nullptr;
    memset(out, 0, 16384);
    for (int i = 0; i < 3; i++) {
        size_t got = 0;
        if (!zip_extract(zip, sz, names[i], out + (size_t)i * 4096, 4096, &got) || got != 4096) {
            Serial.printf("[loader] frogger cpu1 %s failed\n", names[i]);
            free(out); return nullptr;
        }
    }
    return out;
}

/* Load Frogger CPU2 (audio, 3 x 2K = 6K, padded to 8K).
 * Konami's frogger sound ROMs may have D0<->D1 swapped (some MAME sets
 * ship them that way). Upstream romconv checks each file's first 8 bytes
 * against a known signature and D0<->D1-swaps the whole file if matched.
 * Mirror that behaviour per 2K chunk, or the sound CPU runs scrambled
 * opcodes and never EIs — no interrupts, no AY writes, silence. */
extern "C" unsigned char *load_frogger_rom_cpu2_from_zip(const uint8_t *zip, size_t sz) {
    static const char *names[3] = { "frogger.608", "frogger.609", "frogger.610" };
    static const uint8_t swap_sig[8] = { 0x05,0x00,0x22,0x00,0x40,0xc3,0x0b,0x02 };
    unsigned char *out = (unsigned char *)malloc(8192);
    if (!out) return nullptr;
    memset(out, 0, 8192);
    for (int i = 0; i < 3; i++) {
        size_t got = 0;
        unsigned char *chunk = out + (size_t)i * 2048;
        if (!zip_extract(zip, sz, names[i], chunk, 2048, &got) || got != 2048) {
            Serial.printf("[loader] frogger cpu2 %s failed\n", names[i]);
            free(out); return nullptr;
        }
        if (memcmp(chunk, swap_sig, 8) == 0) {
            Serial.printf("[loader] frogger cpu2 %s: D0<->D1 swap applied\n", names[i]);
            for (int j = 0; j < 2048; j++) {
                uint8_t b = chunk[j];
                chunk[j] = (uint8_t)((b & 0xfc) | ((b & 2) >> 1) | ((b & 1) << 1));
            }
        }
    }
    return out;
}

/* Auto-detect + apply Frogger's D0↔D1 bit swap on rom0.
 * The check: if first 16 bytes of rom0 (after swap) equal rom1, apply. */
static void frogger_bit_swap_if_needed(uint8_t *rom0, const uint8_t *rom1, size_t len) {
    for (int i = 0; i < 16; i++) {
        uint8_t swapped = (uint8_t)((rom0[i] & 0xFC) | ((rom0[i] & 1) << 1) | ((rom0[i] & 2) >> 1));
        if (swapped != rom1[i]) return;   /* no swap needed */
    }
    for (size_t i = 0; i < len; i++)
        rom0[i] = (uint8_t)((rom0[i] & 0xFC) | ((rom0[i] & 1) << 1) | ((rom0[i] & 2) >> 1));
}

/* Decode one 8x8 tile from 2 files, 8 bytes each (Frogger's parse_chr_2). */
static void decode_tile_frogger(const uint8_t *d0, const uint8_t *d1, uint16_t out_row[8]) {
    for (int y = 0; y < 8; y++) {
        uint16_t val = 0;
        for (int x = 0; x < 8; x++) {
            int c0 = (d0[7 - x] & (0x80 >> y)) ? 1 : 0;
            int c1 = (d1[7 - x] & (0x80 >> y)) ? 2 : 0;
            int pix = c0 + c1;
            val = (uint16_t)((val >> 2) | (pix << 14));
        }
        out_row[y] = val;
    }
}

/* Load Frogger tilemap: array name `frogger_606` = 256 chars × 8 shorts. */
extern "C" unsigned short *load_frogger_tilemap_from_zip(const uint8_t *zip, size_t sz) {
    RomScratch raw0(2048); if (!raw0) return nullptr;
    RomScratch raw1(2048); if (!raw1) return nullptr;
    size_t got = 0;
    if (!zip_extract(zip, sz, "frogger.606", raw0, 2048, &got) || got != 2048) return nullptr;
    if (!zip_extract(zip, sz, "frogger.607", raw1, 2048, &got) || got != 2048) return nullptr;
    frogger_bit_swap_if_needed(raw0, raw1, 2048);
    unsigned short *out = (unsigned short *)malloc(256 * 8 * sizeof(unsigned short));
    if (!out) return nullptr;
    for (int c = 0; c < 256; c++) decode_tile_frogger(&raw0[8 * c], &raw1[8 * c], &out[c * 8]);
    return out;
}

/* Decode one 16x16 sprite from 32 bytes in each of two files (Frogger fmt). */
static void decode_sprite_frogger(const uint8_t *d0, const uint8_t *d1, uint8_t pix[16][16]) {
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int ym = (y & 7) | ((x & 8) ^ 8);
            int xm = (x & 7) | (y & 8);
            int idx = (xm ^ 7) + ((ym & 8) << 1);
            int c0 = (d0[idx] & (0x80 >> (ym & 7))) ? 1 : 0;
            int c1 = (d1[idx] & (0x80 >> (ym & 7))) ? 2 : 0;
            pix[y][x] = (uint8_t)(c0 + c1);
        }
    }
}

/* Load Frogger sprites: reuses frogger.606/.607, 32 bytes per sprite per file. */
extern "C" unsigned long *load_frogger_spritemap_from_zip(const uint8_t *zip, size_t sz) {
    RomScratch raw0(2048); if (!raw0) return nullptr;
    RomScratch raw1(2048); if (!raw1) return nullptr;
    size_t got = 0;
    if (!zip_extract(zip, sz, "frogger.606", raw0, 2048, &got) || got != 2048) return nullptr;
    if (!zip_extract(zip, sz, "frogger.607", raw1, 2048, &got) || got != 2048) return nullptr;
    frogger_bit_swap_if_needed(raw0, raw1, 2048);
    unsigned long *out = (unsigned long *)malloc(4 * 64 * 16 * sizeof(unsigned long));
    if (!out) return nullptr;
    const bool flip_x[4] = { false, false, true, true };
    const bool flip_y[4] = { false, true,  false, true };
    for (int v = 0; v < 4; v++) {
        for (int s = 0; s < 64; s++) {
            uint8_t pix[16][16];
            decode_sprite_frogger(&raw0[32 * s], &raw1[32 * s], pix);
            uint32_t rows[16];
            pack_sprite_flip(pix, flip_x[v], flip_y[v], rows);
            for (int r = 0; r < 16; r++)
                out[v * 64 * 16 + s * 16 + r] = (unsigned long)rows[r];
        }
    }
    return out;
}

/* ============================== DKONG ============================== */

/* CPU1: 4 x 4K Z80 code (16 KB total). */
extern "C" unsigned char *load_dkong_rom_cpu1_from_zip(const uint8_t *zip, size_t sz) {
    static const char *names[4] = { "c_5et_g.bin", "c_5ct_g.bin", "c_5bt_g.bin", "c_5at_g.bin" };
    unsigned char *out = (unsigned char *)malloc(16384);
    if (!out) return nullptr;
    for (int i = 0; i < 4; i++) {
        size_t got = 0;
        if (!zip_extract(zip, sz, names[i], out + (size_t)i * 4096, 4096, &got) || got != 4096) {
            Serial.printf("[loader] dkong cpu1 %s failed\n", names[i]);
            free(out); return nullptr;
        }
    }
    return out;
}

/* CPU2 (i8048 audio): 2 x 2K = 4K. Layout matches how emulation.c
 * indexes: `dkong_rom_cpu2[2048 + addr + 256 * (p2_state & 7)]` for
 * page-banked access to the second half. */
extern "C" unsigned char *load_dkong_rom_cpu2_from_zip(const uint8_t *zip, size_t sz) {
    static const char *names[2] = { "s_3i_b.bin", "s_3j_b.bin" };
    unsigned char *out = (unsigned char *)malloc(4096);
    if (!out) return nullptr;
    for (int i = 0; i < 2; i++) {
        size_t got = 0;
        if (!zip_extract(zip, sz, names[i], out + (size_t)i * 2048, 2048, &got) || got != 2048) {
            Serial.printf("[loader] dkong cpu2 %s failed\n", names[i]);
            free(out); return nullptr;
        }
    }
    return out;
}

/* Tiles: 2 files of 2K → 256 chars using Frogger-style parse_chr_2. */
extern "C" unsigned short *load_dkong_tilemap_from_zip(const uint8_t *zip, size_t sz) {
    RomScratch raw0(2048); if (!raw0) return nullptr;
    RomScratch raw1(2048); if (!raw1) return nullptr;
    size_t got = 0;
    if (!zip_extract(zip, sz, "v_5h_b.bin", raw0, 2048, &got) || got != 2048) return nullptr;
    if (!zip_extract(zip, sz, "v_3pt.bin",  raw1, 2048, &got) || got != 2048) return nullptr;
    unsigned short *out = (unsigned short *)malloc(256 * 8 * sizeof(unsigned short));
    if (!out) return nullptr;
    /* parse_chr_2: no bit-swap for DKong (only Frogger needs that). */
    for (int c = 0; c < 256; c++) {
        for (int y = 0; y < 8; y++) {
            uint16_t val = 0;
            for (int x = 0; x < 8; x++) {
                int c0 = (raw0[8*c + (7 - x)] & (0x80 >> y)) ? 1 : 0;
                int c1 = (raw1[8*c + (7 - x)] & (0x80 >> y)) ? 2 : 0;
                int pix = c0 + c1;
                val = (uint16_t)((val >> 2) | (pix << 14));
            }
            out[c*8 + y] = val;
        }
    }
    return out;
}

/* Sprites: 4 files of 2K, parts of each 16x16 sprite spread across ALL four.
 * 128 sprites × 4 flip variants × 16 rows × 4 bytes = 32 KB. */
static unsigned long *load_dk_sprites(const uint8_t *zip, size_t sz, const char *const names[4]) {
    RomScratch rbuf(8192); if (!rbuf) return nullptr;
    uint8_t (*r)[2048] = (uint8_t (*)[2048])(uint8_t *)rbuf;
    size_t got = 0;
    for (int i = 0; i < 4; i++) {
        if (!zip_extract(zip, sz, names[i], r[i], 2048, &got) || got != 2048) {
            Serial.printf("[loader] dkong sprite %s failed\n", names[i]);
            return nullptr;
        }
    }
    unsigned long *out = (unsigned long *)malloc(4 * 128 * 16 * sizeof(unsigned long));
    if (!out) return nullptr;
    const bool flip_x[4] = { false, false, true, true };
    const bool flip_y[4] = { false, true,  false, true };
    for (int v = 0; v < 4; v++) {
        for (int s = 0; s < 128; s++) {
            uint8_t pix[16][16];
            const uint8_t *d0 = &r[0][s*16], *d1 = &r[1][s*16];
            const uint8_t *d2 = &r[2][s*16], *d3 = &r[3][s*16];
            for (int y = 0; y < 16; y++) {
                const uint8_t *da = (y < 8) ? d0 : d1;
                const uint8_t *db = (y < 8) ? d2 : d3;
                for (int x = 0; x < 16; x++) {
                    int c0 = (da[15 - x] & (0x80 >> (y & 7))) ? 1 : 0;
                    int c1 = (db[15 - x] & (0x80 >> (y & 7))) ? 2 : 0;
                    pix[y][x] = (uint8_t)(c0 + c1);
                }
            }
            uint32_t rows[16];
            pack_sprite_flip(pix, flip_x[v], flip_y[v], rows);
            for (int r = 0; r < 16; r++)
                out[v*128*16 + s*16 + r] = (unsigned long)rows[r];
        }
    }
    return out;
}

extern "C" unsigned long *load_dkong_spritemap_from_zip(const uint8_t *zip, size_t sz) {
    static const char *const names[4] = { "l_4m_b.bin", "l_4n_b.bin", "l_4r_b.bin", "l_4s_b.bin" };
    return load_dk_sprites(zip, sz, names);
}

/* Cmap: 2 4-bit palette PROMs + 1 lookup PROM. Produces TWO output
 * arrays: dkong_colormap[4][256][4] and dkong_colormap_sprite[4][16][4].
 * Palette uses inverted rrrgggbb encoding (higher = darker on real HW). */
static uint16_t dkong_palette[256];
static void build_dkong_palette(const uint8_t *low4, const uint8_t *high4) {
    for (int i = 0; i < 256; i++) {
        uint8_t c = (uint8_t)((high4[i] << 4) | low4[i]);
        int r = 31 - 31 * ((c >> 5) & 0x7) / 7;
        int g = 63 - 63 * ((c >> 2) & 0x7) / 7;
        int b = 31 - 31 * ((c >> 0) & 0x3) / 3;
        uint16_t rgb = (uint16_t)((r << 11) | (g << 5) | b);
        dkong_palette[i] = (uint16_t)(((rgb & 0xff00) >> 8) | ((rgb & 0xff) << 8));
    }
}

static unsigned short *load_dk_cmap(const uint8_t *zip, size_t sz, const char *low, const char *high, const char *lut) {
    uint8_t plow[256], phigh[256], lookup[256];
    size_t got = 0;
    if (!zip_extract(zip, sz, low,  plow,   256, &got) || got != 256) return nullptr;
    if (!zip_extract(zip, sz, high, phigh,  256, &got) || got != 256) return nullptr;
    if (!zip_extract(zip, sz, lut,  lookup, 256, &got) || got != 256) return nullptr;
    build_dkong_palette(plow, phigh);
    /* [4 screens][256 groups][4 shorts] */
    unsigned short *out = (unsigned short *)malloc(4 * 256 * 4 * sizeof(unsigned short));
    if (!out) return nullptr;
    for (int s = 0; s < 4; s++)
        for (int g = 0; g < 256; g++) {
            int idx = lookup[g] + 16 * s;
            for (int j = 0; j < 4; j++)
                out[s*256*4 + g*4 + j] = dkong_palette[4*idx + j];
        }
    return out;
}

extern "C" unsigned short *load_dkong_cmap_from_zip(const uint8_t *zip, size_t sz) {
    return load_dk_cmap(zip, sz, "c-2k.bpr", "c-2j.bpr", "v-5e.bpr");
}

extern "C" unsigned short *load_dkong_cmap_sprite_from_zip(const uint8_t *zip, size_t sz) {
    /* Depends on dkong_palette already being built by load_dkong_cmap. */
    unsigned short *out = (unsigned short *)malloc(4 * 16 * 4 * sizeof(unsigned short));
    if (!out) return nullptr;
    for (int s = 0; s < 4; s++)
        for (int i = 0; i < 16; i++) {
            int idx = i + 16 * s;
            for (int j = 0; j < 4; j++)
                out[s*16*4 + i*4 + j] = dkong_palette[4*idx + j];
        }
    return out;
}

/* Frogger colormap: 32-byte PROM pr-91.6l — 32 palette entries laid
 * out as [8][4] to match how frogger.h indexes. */
extern "C" unsigned short *load_frogger_cmap_from_zip(const uint8_t *zip, size_t sz) {
    uint8_t prom[32];
    size_t got = 0;
    if (!zip_extract(zip, sz, "pr-91.6l", prom, 32, &got) || got != 32) return nullptr;
    unsigned short *out = (unsigned short *)malloc(32 * sizeof(unsigned short));   /* 8*4 */
    if (!out) return nullptr;
    for (int i = 0; i < 32; i++) {
        uint8_t c = prom[i];
        int b = 31 * ((c >> 6) & 0x3) / 3;
        int g = 63 * ((c >> 3) & 0x7) / 7;
        int r = 31 * ((c >> 0) & 0x7) / 7;
        uint16_t rgb = (uint16_t)((r << 11) | (g << 5) | b);
        out[i] = (uint16_t)(((rgb & 0xff00) >> 8) | ((rgb & 0xff) << 8));
    }
    return out;
}

/* ============================== DK JR. ============================== */
/* US parent set (dkongjr) or Japan set (dkongjrj): same layout, different
 * file names (and program / tile contents). Loads into the DK pointers. */
struct DkjrSet {
    const char *cpu1[3], *snd, *tile[2], *spr[4], *pal_low, *pal_high, *lut;
};
static const DkjrSet DKJR_US = {
    { "djr1-c_5b_f-2.5b", "djr1-c_5c_f-2.5c", "djr1-c_5e_f-2.5e" }, "djr1-c_3h.3h",
    { "djr1-v.3n", "djr1-v.3p" }, { "djr1-v_7c.7c", "djr1-v_7d.7d", "djr1-v_7e.7e", "djr1-v_7f.7f" },
    "djr1-c-2e.2e", "djr1-c-2f.2f", "djr1-v-2n.2n" };
static const DkjrSet DKJR_JP = {
    { "c_5ba.bin", "c_5ca.bin", "c_5ea.bin" }, "c_3h.bin",
    { "v_3na.bin", "v_3pa.bin" }, { "v_7c.bin", "v_7d.bin", "v_7e.bin", "v_7f.bin" },
    "c-2e.bpr", "c-2f.bpr", "v-2n.bpr" };
static const DkjrSet *dkjr_set(bool japan) { return japan ? &DKJR_JP : &DKJR_US; }

/* Main CPU: 3 x 8 KB chips, each spread over the 24 KB map in 2 / 4 KB
 * pieces (MAME ROM_LOAD + ROM_CONTINUE). 32 KB buffer = fetch mask. */
extern "C" unsigned char *load_dkongjr_rom_cpu1_from_zip(const uint8_t *zip, size_t sz, bool japan) {
    static const struct { uint16_t dst[4]; uint16_t len; } map[3] = {
        { { 0x0000, 0x3000, 0xffff, 0xffff }, 0x1000 },
        { { 0x2000, 0x4800, 0x1000, 0x5800 }, 0x0800 },
        { { 0x4000, 0x2800, 0x5000, 0x1800 }, 0x0800 },
    };
    RomScratch chip(8192); if (!chip) return nullptr;
    unsigned char *out = (unsigned char *)malloc(0x8000);
    if (!out) return nullptr;
    memset(out, 0xff, 0x8000);
    for (int c = 0; c < 3; c++) {
        const char *name = dkjr_set(japan)->cpu1[c];
        size_t got = 0;
        if (!zip_extract(zip, sz, name, chip, 8192, &got) || got != 8192) {
            Serial.printf("[loader] dkongjr cpu1 %s failed\n", name);
            free(out); return nullptr;
        }
        for (int p = 0; p < 4 && map[c].dst[p] != 0xffff; p++)
            memcpy(out + map[c].dst[p], chip + p * map[c].len, map[c].len);
    }
    return out;
}

/* i8048: one 4 KB ROM (same layout as DK's two 2 KB halves). */
extern "C" unsigned char *load_dkongjr_rom_cpu2_from_zip(const uint8_t *zip, size_t sz, bool japan) {
    unsigned char *out = (unsigned char *)malloc(4096);
    if (!out) return nullptr;
    size_t got = 0;
    if (!zip_extract(zip, sz, dkjr_set(japan)->snd, out, 4096, &got) || got != 4096) { free(out); return nullptr; }
    return out;
}

/* 512 tiles (2 banks) from 2 x 4 KB planes. */
extern "C" unsigned short *load_dkongjr_tilemap_from_zip(const uint8_t *zip, size_t sz, bool japan) {
    RomScratch raw0(4096); if (!raw0) return nullptr;
    RomScratch raw1(4096); if (!raw1) return nullptr;
    size_t got = 0;
    if (!zip_extract(zip, sz, dkjr_set(japan)->tile[0], raw0, 4096, &got) || got != 4096) return nullptr;
    if (!zip_extract(zip, sz, dkjr_set(japan)->tile[1], raw1, 4096, &got) || got != 4096) return nullptr;
    unsigned short *out = (unsigned short *)malloc(512 * 8 * sizeof(unsigned short));
    if (!out) return nullptr;
    for (int c = 0; c < 512; c++) decode_tile_frogger(&raw0[8 * c], &raw1[8 * c], &out[c * 8]);
    return out;
}

/* Sprites, palette and lookup: DK's decoders with DK Jr.'s files. */
extern "C" unsigned long *load_dkongjr_spritemap_from_zip(const uint8_t *zip, size_t sz, bool japan) {
    return load_dk_sprites(zip, sz, dkjr_set(japan)->spr);
}
extern "C" unsigned short *load_dkongjr_cmap_from_zip(const uint8_t *zip, size_t sz, bool japan) {
    const DkjrSet *s = dkjr_set(japan);
    return load_dk_cmap(zip, sz, s->pal_low, s->pal_high, s->lut);
}

/* ============================== DIGDUG ============================== */

/* Load a variable count of same-sized ROM chunks concatenated into one
 * buffer. Bail if any chunk is missing or wrong size. */
static unsigned char *load_concat(const uint8_t *zip, size_t sz,
                                  const char *const *names, int count,
                                  size_t chunk_sz, const char *tag) {
    unsigned char *out = (unsigned char *)malloc(chunk_sz * (size_t)count);
    if (!out) return nullptr;
    for (int i = 0; i < count; i++) {
        size_t got = 0;
        if (!zip_extract(zip, sz, names[i], out + (size_t)i * chunk_sz, chunk_sz, &got) || got != chunk_sz) {
            Serial.printf("[loader] %s %s failed (got=%u expected=%u)\n",
                          tag, names[i], (unsigned)got, (unsigned)chunk_sz);
            free(out); return nullptr;
        }
    }
    return out;
}

/* CPU1: 4 × 4 KB = 16 KB. */
extern "C" unsigned char *load_digdug_rom_cpu1_from_zip(const uint8_t *zip, size_t sz) {
    static const char *names[4] = { "dd1a.1", "dd1a.2", "dd1a.3", "dd1a.4" };
    return load_concat(zip, sz, names, 4, 4096, "digdug cpu1");
}
/* CPU2: 2 × 4 KB = 8 KB. */
extern "C" unsigned char *load_digdug_rom_cpu2_from_zip(const uint8_t *zip, size_t sz) {
    static const char *names[2] = { "dd1a.5", "dd1a.6" };
    return load_concat(zip, sz, names, 2, 4096, "digdug cpu2");
}
/* CPU3 (sound): 1 × 4 KB. */
extern "C" unsigned char *load_digdug_rom_cpu3_from_zip(const uint8_t *zip, size_t sz) {
    static const char *names[1] = { "dd1.7" };
    return load_concat(zip, sz, names, 1, 4096, "digdug cpu3");
}
/* Playfield data: dd1.10b, 4 KB direct copy — 4 playfield selections × 1 KB. */
extern "C" unsigned char *load_digdug_playfield_from_zip(const uint8_t *zip, size_t sz) {
    static const char *names[1] = { "dd1.10b" };
    return load_concat(zip, sz, names, 1, 4096, "digdug playfield");
}

/* Digdug 1bpp tile parser: 8 bytes → 8 rows × 8 pixels, each pixel either
 * colour 0 or 3 (2 bpp packed same way as pacman/galaga). Row `y`, column `x`:
 * pixel = bit y of data[7-x], mapped 0→0 / 1→3. */
static void decode_tile_digdug(const uint8_t *data, uint16_t out_row[8]) {
    for (int y = 0; y < 8; y++) {
        uint16_t val = 0;
        for (int x = 0; x < 8; x++) {
            uint8_t b = data[7 - x];
            uint16_t px = (b & (1 << y)) ? 3 : 0;
            val = (uint16_t)((val >> 2) | (px << 14));
        }
        out_row[y] = val;
    }
}

/* Char tiles (dd1.9) → 128 tiles × 8 shorts. MAME digdug ships dd1.9 as
 * 2 KB (256 chars) but upstream only decodes the first 128 chars. Give
 * zip_extract enough buffer for whichever size the ROM actually is. */
extern "C" unsigned short *load_digdug_tilemap_from_zip(const uint8_t *zip, size_t sz) {
    RomScratch raw(4096); if (!raw) return nullptr;
    size_t got = 0;
    if (!zip_extract(zip, sz, "dd1.9", raw, 4096, &got) || got < 1024) {
        Serial.printf("[loader] digdug tilemap dd1.9 failed (got=%u)\n", (unsigned)got);
        return nullptr;
    }
    unsigned short *out = (unsigned short *)malloc(128 * 8 * sizeof(unsigned short));
    if (!out) return nullptr;
    for (int c = 0; c < 128; c++) decode_tile_digdug(&raw[8 * c], &out[c * 8]);
    return out;
}
/* Playfield tiles (dd1.11) — format depends on ROM size, matching upstream
 * parse_tilemap: 4 KB → pacman/galaga 2bpp (16 B/char, 256 chars); anything
 * smaller → digdug 1bpp (8 B/char, 128 chars). dd1.11 is 4 KB in the
 * mainline digdug set but some sets ship 2 KB — handle both. Playfield
 * indices in digdug_blit_tile go up to 8 bits, so allocate 256 entries. */
extern "C" unsigned short *load_digdug_pftiles_from_zip(const uint8_t *zip, size_t sz) {
    RomScratch raw(4096); if (!raw) return nullptr;
    size_t got = 0;
    if (!zip_extract(zip, sz, "dd1.11", raw, 4096, &got) || got < 1024) {
        Serial.printf("[loader] digdug pftiles dd1.11 failed (got=%u)\n", (unsigned)got);
        return nullptr;
    }
    unsigned short *out = (unsigned short *)malloc(256 * 8 * sizeof(unsigned short));
    if (!out) return nullptr;
    memset(out, 0, 256 * 8 * sizeof(unsigned short));
    if (got >= 4096) {
        for (int c = 0; c < 256; c++) decode_tile_pac(&raw[16 * c], &out[c * 8]);
    } else {
        for (int c = 0; c < 128; c++) decode_tile_digdug(&raw[8 * c], &out[c * 8]);
    }
    return out;
}

/* Sprites: 4 × 4 KB files → 4 × 64 = 256 sprites, expanded to 4 flip
 * variants. Reuses galaga's decoder (same bit-plane layout, no row rotation)
 * and pack_sprite_flip. Total = 4 × 256 × 16 × 4 bytes = 64 KB. */
extern "C" unsigned long *load_digdug_spritemap_from_zip(const uint8_t *zip, size_t sz) {
    static const char *names[4] = { "dd1.15", "dd1.14", "dd1.13", "dd1.12" };
    RomScratch raw(16384); if (!raw) return nullptr;   /* 4 x 4 KB */
    size_t got = 0;
    for (int i = 0; i < 4; i++) {
        if (!zip_extract(zip, sz, names[i], raw + (size_t)i * 4096, 4096, &got) || got != 4096) {
            Serial.printf("[loader] digdug sprite %s failed\n", names[i]); return nullptr;
        }
    }
    unsigned long *out = (unsigned long *)malloc(4 * 256 * 16 * sizeof(unsigned long));
    if (!out) return nullptr;
    const bool flip_x[4] = { false, false, true, true };
    const bool flip_y[4] = { false, true,  false, true };
    for (int v = 0; v < 4; v++) {
        for (int s = 0; s < 256; s++) {
            uint8_t pix[16][16];
            decode_sprite_galaga(&raw[64 * s], pix);
            uint32_t rows[16];
            pack_sprite_flip(pix, flip_x[v], flip_y[v], rows);
            for (int r = 0; r < 16; r++)
                out[v * 256 * 16 + s * 16 + r] = (unsigned long)rows[r];
        }
    }
    return out;
}

/* Digdug palette PROM 136007.113 (32 bytes) — 8-bit RGB332-ish; convert
 * to swapped 16-bit RGB565 the same way galaga/frogger do. Optionally
 * slice at `offset` for 16 colours. */
static bool read_digdug_palette(const uint8_t *zip, size_t sz, uint16_t palette[32]) {
    uint8_t prom[32];
    size_t got = 0;
    if (!zip_extract(zip, sz, "136007.113", prom, 32, &got) || got != 32) return false;
    for (int i = 0; i < 32; i++) {
        uint8_t c = prom[i];
        int b = 31 * ((c >> 6) & 0x3) / 3;
        int g = 63 * ((c >> 3) & 0x7) / 7;
        int r = 31 * ((c >> 0) & 0x7) / 7;
        uint16_t rgb = (uint16_t)((r << 11) | (g << 5) | b);
        palette[i] = (uint16_t)(((rgb & 0xff00) >> 8) | ((rgb & 0xff) << 8));
    }
    return true;
}

/* Build a colormap-lookup table: 64 groups × 4 palette-index bytes from
 * the given lookup PROM (256 bytes). The palette slice starts at `offset`. */
static unsigned short *build_digdug_cmap(const uint8_t *zip, size_t sz,
                                         const char *lookup_prom, int offset) {
    uint16_t palette[32];
    if (!read_digdug_palette(zip, sz, palette)) return nullptr;
    uint8_t lookup[256];
    size_t got = 0;
    if (!zip_extract(zip, sz, lookup_prom, lookup, 256, &got) || got != 256) return nullptr;
    unsigned short *out = (unsigned short *)malloc(64 * 4 * sizeof(unsigned short));
    if (!out) return nullptr;
    for (int i = 0; i < 64; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = palette[offset + (lookup[i * 4 + j] & 0x0F)];
    return out;
}
extern "C" unsigned short *load_digdug_cmap_tiles_from_zip(const uint8_t *zip, size_t sz) {
    return build_digdug_cmap(zip, sz, "136007.112", 0);
}
extern "C" unsigned short *load_digdug_cmap_sprites_from_zip(const uint8_t *zip, size_t sz) {
    return build_digdug_cmap(zip, sz, "136007.111", 16);
}
/* digdug_colormaps — the whole 32-entry palette, indexed as [8][4]. */
extern "C" unsigned short *load_digdug_cmap_from_zip(const uint8_t *zip, size_t sz) {
    uint16_t palette[32];
    if (!read_digdug_palette(zip, sz, palette)) return nullptr;
    unsigned short *out = (unsigned short *)malloc(32 * sizeof(unsigned short));
    if (!out) return nullptr;
    for (int i = 0; i < 32; i++) out[i] = palette[i];
    return out;
}

/* Wavetable: two 256-byte PROMs concatenated as-is → 16 waveforms × 32
 * samples of signed 8-bit (each byte holds 0..15, centred to -7..+8).
 * soundregs[…]&0x0f indexes into 16 entries. */
extern "C" signed char *load_digdug_wavetable_from_zip(const uint8_t *zip, size_t sz) {
    uint8_t raw[512];
    size_t got = 0;
    if (!zip_extract(zip, sz, "136007.110", raw + 0,   256, &got) || got != 256) return nullptr;
    if (!zip_extract(zip, sz, "136007.109", raw + 256, 256, &got) || got != 256) return nullptr;
    signed char *out = (signed char *)malloc(16 * 32);
    if (!out) return nullptr;
    for (int i = 0; i < 512; i++) out[i] = (signed char)((int)raw[i] - 7);
    return out;
}

/* ============================== SUPER COBRA ============================== */

/* Super Cobra and Scramble share these loaders; the set picks the files.
 * gfx: MAME loads the MSB plane first (Galaxian layout) — 5h on Super
 * Cobra, c2.5f on Scramble. */
struct ScobraSet {
    const char *cpu1[8]; int n1; size_t sz1;   /* main CPU chunks */
    const char *cpu2[3];                       /* sound CPU, 3 x 2 KB */
    const char *gfx_lsb, *gfx_msb, *prom;
};
static const ScobraSet SCOBRA_SET = {
    { "epr1265.2c", "2e", "epr1267.2f", "2h", "epr1269.2j", "2l" }, 6, 4096,
    { "5c", "5d", "5e" }, "epr1273.5f", "epr1274.5h", "82s123.6e" };
static const ScobraSet SCRAMBLE_SET = {
    { "s1.2d", "s2.2e", "s3.2f", "s4.2h", "s5.2j", "s6.2l", "s7.2m", "s8.2p" }, 8, 2048,
    { "ot1.5c", "ot2.5d", "ot3.5e" }, "c1.5h", "c2.5f", "c01s.6e" };
static const ScobraSet *scobra_set(bool scramble) { return scramble ? &SCRAMBLE_SET : &SCOBRA_SET; }

/* CPU1: the chunks from 0x0000, padded to the 32 KB the fetch mask covers. */
extern "C" unsigned char *load_scobra_rom_cpu1_from_zip(const uint8_t *zip, size_t sz, bool scramble) {
    const ScobraSet *set = scobra_set(scramble);
    unsigned char *out = (unsigned char *)malloc(0x8000);
    if (!out) return nullptr;
    memset(out, 0xff, 0x8000);
    for (int i = 0; i < set->n1; i++) {
        size_t got = 0;
        if (!zip_extract(zip, sz, set->cpu1[i], out + (size_t)i * set->sz1, set->sz1, &got) || got != set->sz1) {
            Serial.printf("[loader] scobra cpu1 %s failed\n", set->cpu1[i]);
            free(out); return nullptr;
        }
    }
    return out;
}

/* CPU2 (sound): 3 x 2 KB, padded to 8 KB. */
extern "C" unsigned char *load_scobra_rom_cpu2_from_zip(const uint8_t *zip, size_t sz, bool scramble) {
    const ScobraSet *set = scobra_set(scramble);
    unsigned char *out = (unsigned char *)malloc(0x2000);
    if (!out) return nullptr;
    memset(out, 0xff, 0x2000);
    for (int i = 0; i < 3; i++) {
        size_t got = 0;
        if (!zip_extract(zip, sz, set->cpu2[i], out + (size_t)i * 2048, 2048, &got) || got != 2048) {
            Serial.printf("[loader] scobra cpu2 %s failed\n", set->cpu2[i]);
            free(out); return nullptr;
        }
    }
    return out;
}

/* The Frogger decoders put their first file in plane 0 (LSB). */
static bool scobra_gfx(const uint8_t *zip, size_t sz, bool scramble, uint8_t *p0, uint8_t *p1) {
    const ScobraSet *set = scobra_set(scramble);
    size_t got = 0;
    return zip_extract(zip, sz, set->gfx_lsb, p0, 2048, &got) && got == 2048 &&
           zip_extract(zip, sz, set->gfx_msb, p1, 2048, &got) && got == 2048;
}

/* 256 chars x 8 rows, 2bpp. */
extern "C" unsigned short *load_scobra_tilemap_from_zip(const uint8_t *zip, size_t sz, bool scramble) {
    RomScratch p0(2048); if (!p0) return nullptr;
    RomScratch p1(2048); if (!p1) return nullptr;
    if (!scobra_gfx(zip, sz, scramble, p0, p1)) return nullptr;
    unsigned short *out = (unsigned short *)malloc(256 * 8 * sizeof(unsigned short));
    if (!out) return nullptr;
    for (int c = 0; c < 256; c++) decode_tile_frogger(&p0[8 * c], &p1[8 * c], &out[c * 8]);
    return out;
}

/* 64 sprites 16x16 x 4 flip variants (none, y, x, xy) = 16 KB. */
extern "C" unsigned long *load_scobra_spritemap_from_zip(const uint8_t *zip, size_t sz, bool scramble) {
    RomScratch p0(2048); if (!p0) return nullptr;
    RomScratch p1(2048); if (!p1) return nullptr;
    if (!scobra_gfx(zip, sz, scramble, p0, p1)) return nullptr;
    unsigned long *out = (unsigned long *)malloc(4 * 64 * 16 * sizeof(unsigned long));
    if (!out) return nullptr;
    const bool flip_x[4] = { false, false, true, true };
    const bool flip_y[4] = { false, true,  false, true };
    for (int s = 0; s < 64; s++) {
        uint8_t pix[16][16];
        decode_sprite_frogger(&p0[32 * s], &p1[32 * s], pix);
        for (int v = 0; v < 4; v++) {
            uint32_t rows[16];
            pack_sprite_flip(pix, flip_x[v], flip_y[v], rows);
            for (int r = 0; r < 16; r++) out[v * 64 * 16 + s * 16 + r] = (unsigned long)rows[r];
        }
    }
    return out;
}

/* Colour PROM: 32 x bbgggrrr through the Galaxian resistor network,
 * 8 palettes x 4 colours, byte-swapped RGB565. */
extern "C" unsigned short *load_scobra_cmap_from_zip(const uint8_t *zip, size_t sz, bool scramble) {
    uint8_t prom[32];
    size_t got = 0;
    if (!zip_extract(zip, sz, scobra_set(scramble)->prom, prom, 32, &got) || got != 32) return nullptr;
    unsigned short *out = (unsigned short *)malloc(32 * sizeof(unsigned short));
    if (!out) return nullptr;
    for (int i = 0; i < 32; i++) {
        uint8_t c = prom[i];
        int r = 0x21 * ((c >> 0) & 1) + 0x47 * ((c >> 1) & 1) + 0x97 * ((c >> 2) & 1);
        int g = 0x21 * ((c >> 3) & 1) + 0x47 * ((c >> 4) & 1) + 0x97 * ((c >> 5) & 1);
        int b = 0x4f * ((c >> 6) & 1) + 0xa8 * ((c >> 7) & 1);
        uint16_t rgb = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        out[i] = (uint16_t)((rgb >> 8) | (rgb << 8));
    }
    return out;
}
