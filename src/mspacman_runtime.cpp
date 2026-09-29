/* mspacman_runtime.cpp — Ms. Pac-Man runtime loader + F2 aux-board fetch.
 *
 * Ms. Pac-Man ran on stock Pac-Man hardware plus a small daughterboard
 * ("F2") between the Z80 and its ROMs. The board:
 *   - Replaces pacman.6j with a decoded copy of u7 at 0x3000-0x3FFF
 *   - Adds a decoded 8 KB aux ROM at 0x8000-0x9FFF (from u5+u6)
 *   - Mirrors half of the aux ROM at 0xA000-0xBFFF
 *   - Enables the Ms. Pac-Man map on any access in 0x3FF8-0x3FFF (ENABLE trap)
 *   - Reverts to plain Pac-Man on any access in seven small "disable" windows
 *
 * We port GalaginoPlus's build-time decoder (bit-swap on data, addr-swap on
 * A11/A12 lines, 40 JMP-stub patches) to run once at load time. The runtime
 * flip lives in mspacman_fetch(), called from OpZ80_INL / pacman_RdZ80 when
 * machine == MCH_MSPACMAN.
 *
 * Graphics reuse Pac-Man's render path; we allocate fresh tilemap/spritemap
 * arrays from Ms. Pac-Man's 5e/5f and lay two per-position delta tables
 * (mspacman_til_patch, mspacman_spr_patch) on top, then reassign
 * pacman_5e / pacman_sprites so pacman_render_row picks them up untouched.
 *
 * mspacman.zip is fully self-contained: Midway pacman.6e/6f/6h/6j + F2 aux
 * (u5/u6/u7) + gfx (5e/5f) + all PROMs. No puckman.zip dependency. */

#include "rom_scratch.h"
#include <Arduino.h>
#include <SD.h>
#include <stdint.h>
#include <string.h>

#include "mspacman_til_patch.h"
#include "mspacman_spr_patch.h"

/* We reuse Pac-Man's runtime pointers so pacman_render_row() can draw
 * Ms. Pac-Man without any changes. Every one of these gets reassigned
 * from mspacman.zip contents. */
extern "C" unsigned char  *pacman_rom;
extern "C" unsigned short (*pacman_5e)[8];
extern "C" unsigned short (*pacman_colormap)[4];
extern "C" unsigned long  (*pacman_sprites)[64][16];
extern "C" signed char    (*pacman_wavetable)[32];

/* Runtime state for the F2 board. */
extern "C" {
    unsigned char *mspacman_pacrom = nullptr;   /* 16 KB, decoded */
    unsigned char *mspacman_auxrom = nullptr;   /*  8 KB, decoded */
    unsigned char  mspacman_decode = 0;         /* 0 = pacman, 1 = mspacman */
}

/* ------------------------------------------------------------------ */
/* F2 fetch — hot path, called from OpZ80_INL and pacman_RdZ80.       */
/* ------------------------------------------------------------------ */

static inline bool is_disable_trap(unsigned short a) {
    return (a >= 0x0038 && a <= 0x003F) ||
           (a >= 0x03B0 && a <= 0x03B7) ||
           (a >= 0x1600 && a <= 0x1607) ||
           (a >= 0x2120 && a <= 0x2127) ||
           (a >= 0x3FF0 && a <= 0x3FF7) ||
           (a >= 0x8000 && a <= 0x8007) ||
           (a >= 0x97F0 && a <= 0x97F7);
}

extern "C" unsigned char mspacman_fetch(unsigned short Addr) {
    if (Addr >= 0x3FF8 && Addr <= 0x3FFF) {
        mspacman_decode = 1;
        return mspacman_pacrom[Addr];
    }
    if (is_disable_trap(Addr)) {
        mspacman_decode = 0;
        return pacman_rom[Addr & 0x3FFF];
    }
    if (Addr < 0x4000)
        return mspacman_decode ? mspacman_pacrom[Addr] : pacman_rom[Addr];
    if (Addr >= 0x8000 && Addr < 0xA000)
        return mspacman_decode ? mspacman_auxrom[Addr - 0x8000] : pacman_rom[Addr & 0x3FFF];
    if (Addr >= 0xA000 && Addr < 0xC000)
        return mspacman_decode ? mspacman_pacrom[Addr - 0x8000] : pacman_rom[Addr & 0x3FFF];
    return 0xFF;
}

/* ------------------------------------------------------------------ */
/* Load-time decoders (from GalaginoPlus mspacman_rom_convert.py).    */
/* ------------------------------------------------------------------ */

static uint8_t decode_byte(uint8_t b) {
    return (uint8_t)(
        (((b >> 0) & 1) << 7) | (((b >> 4) & 1) << 6) | (((b >> 5) & 1) << 5) |
        (((b >> 7) & 1) << 4) | (((b >> 6) & 1) << 3) | (((b >> 3) & 1) << 2) |
        (((b >> 2) & 1) << 1) | (((b >> 1) & 1) << 0));
}
static uint16_t addr_swap11(uint16_t i) {
    return (uint16_t)(
        (((i >>  8) & 1) << 10) | (((i >>  7) & 1) <<  9) | (((i >>  5) & 1) <<  8) |
        (((i >>  9) & 1) <<  7) | (((i >> 10) & 1) <<  6) | (((i >>  6) & 1) <<  5) |
        (((i >>  3) & 1) <<  4) | (((i >>  4) & 1) <<  3) | (((i >>  2) & 1) <<  2) |
        (((i >>  1) & 1) <<  1) | (((i >>  0) & 1) <<  0));
}
static uint16_t addr_swap12(uint16_t i) {
    return (uint16_t)(
        (((i >> 11) & 1) << 11) | (((i >>  3) & 1) << 10) | (((i >>  7) & 1) <<  9) |
        (((i >>  9) & 1) <<  8) | (((i >> 10) & 1) <<  7) | (((i >>  8) & 1) <<  6) |
        (((i >>  6) & 1) <<  5) | (((i >>  5) & 1) <<  4) | (((i >>  4) & 1) <<  3) |
        (((i >>  2) & 1) <<  2) | (((i >>  1) & 1) <<  1) | (((i >>  0) & 1) <<  0));
}

static const uint16_t f2_patches[40][2] = {
    {0x0410,0x8008},{0x08E0,0x81D8},{0x0A30,0x8118},{0x0BD0,0x80D8},
    {0x0C20,0x8120},{0x0E58,0x8168},{0x0EA8,0x8198},{0x1000,0x8020},
    {0x1008,0x8010},{0x1288,0x8098},{0x1348,0x8048},{0x1688,0x8088},
    {0x16B0,0x8188},{0x16D8,0x80C8},{0x16F8,0x81C8},{0x19A8,0x80A8},
    {0x19B8,0x81A8},{0x2060,0x8148},{0x2108,0x8018},{0x21A0,0x81A0},
    {0x2298,0x80A0},{0x23E0,0x80E8},{0x2418,0x8000},{0x2448,0x8058},
    {0x2470,0x8140},{0x2488,0x8080},{0x24B0,0x8180},{0x24D8,0x80C0},
    {0x24F8,0x81C0},{0x2748,0x8050},{0x2780,0x8090},{0x27B8,0x8190},
    {0x2800,0x8028},{0x2B20,0x8100},{0x2B30,0x8110},{0x2BF0,0x81D0},
    {0x2CC0,0x80D0},{0x2CD8,0x80E0},{0x2CF0,0x81E0},{0x2D60,0x8160}
};

/* zip_extract lives in mame_rom_loader.cpp as a static function. Public
 * wrapper is added when Ms. Pac-Man is compiled. */
extern "C" bool zip_extract_public(const uint8_t *zip, size_t zip_size,
                                   const char *want_name,
                                   uint8_t *dst, size_t dst_capacity,
                                   size_t *out_size);

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

static bool load_base_pacman_rom(const uint8_t *msz, size_t msz_sz) {
    static const char *names[4] = { "pacman.6e", "pacman.6f", "pacman.6h", "pacman.6j" };
    unsigned char *rom = (unsigned char *)malloc(0x4000);
    if (!rom) return false;
    for (int i = 0; i < 4; i++) {
        size_t got = 0;
        if (!zip_extract_public(msz, msz_sz, names[i], rom + i * 0x1000, 0x1000, &got) || got != 0x1000) {
            Serial.printf("[mspacman] %s failed (got=%u)\n", names[i], (unsigned)got);
            free(rom); return false;
        }
    }
    pacman_rom = rom;
    return true;
}

static bool build_pacrom_auxrom(const uint8_t *msz, size_t msz_sz) {
    RomScratch u5(0x800); if (!u5) return false;
    RomScratch u6(0x1000); if (!u6) return false;
    RomScratch u7(0x1000); if (!u7) return false;
    size_t got = 0;
    if (!zip_extract_public(msz, msz_sz, "u5", u5, 0x800, &got) || got != 0x800) {
        Serial.printf("[mspacman] u5 failed (got=%u)\n", (unsigned)got); return false;
    }
    if (!zip_extract_public(msz, msz_sz, "u6", u6, 0x1000, &got) || got != 0x1000) {
        Serial.printf("[mspacman] u6 failed (got=%u)\n", (unsigned)got); return false;
    }
    if (!zip_extract_public(msz, msz_sz, "u7", u7, 0x1000, &got) || got != 0x1000) {
        Serial.printf("[mspacman] u7 failed (got=%u)\n", (unsigned)got); return false;
    }

    mspacman_pacrom = (unsigned char *)malloc(0x4000);
    if (!mspacman_pacrom) return false;
    memcpy(mspacman_pacrom, pacman_rom, 0x3000);
    for (int i = 0; i < 0x1000; i++)
        mspacman_pacrom[0x3000 + i] = decode_byte(u7[addr_swap12((uint16_t)i) & 0xFFF]);

    mspacman_auxrom = (unsigned char *)malloc(0x2000);
    if (!mspacman_auxrom) return false;
    for (int i = 0; i < 0x800; i++) {
        mspacman_auxrom[0x0000 + i] = decode_byte(u5[addr_swap11((uint16_t)i) & 0x7FF]);
        mspacman_auxrom[0x0800 + i] = decode_byte(u6[(addr_swap12((uint16_t)i) & 0x7FF) + 0x800]);
        mspacman_auxrom[0x1000 + i] = decode_byte(u6[addr_swap12((uint16_t)i) & 0x7FF]);
        mspacman_auxrom[0x1800 + i] = pacman_rom[0x1800 + i];
    }

    for (int p = 0; p < 40; p++) {
        uint16_t dst = f2_patches[p][0];
        uint16_t src = f2_patches[p][1];
        for (int k = 0; k < 8; k++)
            mspacman_pacrom[dst + k] = mspacman_auxrom[(src - 0x8000) + k];
    }
    return true;
}

static unsigned short *build_colormap(const uint8_t *msz, size_t msz_sz) {
    uint8_t palette_prom[32], lookup_prom[256];
    size_t got = 0;
    if (!zip_extract_public(msz, msz_sz, "82s123.7f", palette_prom, 32, &got) || got != 32) {
        Serial.println("[mspacman] 82s123.7f failed"); return nullptr;
    }
    if (!zip_extract_public(msz, msz_sz, "82s126.4a", lookup_prom, 256, &got) || got != 256) {
        Serial.println("[mspacman] 82s126.4a failed"); return nullptr;
    }
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
            out[i * 4 + j] = palette[lookup_prom[i * 4 + j] & 0x0F];
    return out;
}

static signed char *build_wavetable(const uint8_t *msz, size_t msz_sz) {
    /* Only 82s126.1m has real wave data (8 waves × 32 samples = 256 B).
     * 82s126.3m is padding on Pac-Man/Ms. Pac-Man hardware. Real Namco WSG3
     * has 8 waveforms; the 4-bit wave-select register in soundregs must be
     * masked to 3 bits (& 0x07) so writes of 0x8-0xF wrap to 0-7. That
     * masking lives in galagino.cpp's wavetable dispatch. */
    static const char *names[2] = { "82s126.1m", "82s126.3m" };
    signed char *out = (signed char *)malloc(16 * 32);
    if (!out) return nullptr;
    for (int p = 0; p < 2; p++) {
        uint8_t raw[256];
        size_t got = 0;
        if (!zip_extract_public(msz, msz_sz, names[p], raw, 256, &got) || got != 256) {
            Serial.printf("[mspacman] %s failed\n", names[p]);
            free(out); return nullptr;
        }
        for (int i = 0; i < 256; i++)
            out[p * 256 + i] = (signed char)((int)raw[i] - 7);
    }
    return out;
}

static unsigned short (*build_tilemap(const uint8_t *msz, size_t msz_sz))[8] {
    RomScratch g5e(0x1000); if (!g5e) return nullptr;
    RomScratch g5f(0x1000); if (!g5f) return nullptr;
    size_t got = 0;
    if (!zip_extract_public(msz, msz_sz, "5e", g5e, 0x1000, &got) || got < 0x800) {
        Serial.printf("[mspacman] 5e failed (got=%u)\n", (unsigned)got); return nullptr;
    }
    if (!zip_extract_public(msz, msz_sz, "5f", g5f, 0x1000, &got) || got < 0x800) {
        Serial.printf("[mspacman] 5f failed (got=%u)\n", (unsigned)got); return nullptr;
    }
    unsigned short *out = (unsigned short *)malloc(256 * 8 * sizeof(unsigned short));
    if (!out) return nullptr;
    int idx = 0;
    for (int t = 0; t < 256; t++) {
        for (int row = 0; row < 8; row++) {
            uint8_t b0 = g5e[t * 8 + row], b1 = g5f[t * 8 + row];
            uint16_t val = 0;
            for (int bit = 0; bit < 8; bit++) {
                int p0 = (b0 >> (7 - bit)) & 1;
                int p1 = (b1 >> (7 - bit)) & 1;
                val |= (uint16_t)((((p1 << 1) | p0) & 3) << (bit * 2));
            }
            out[idx] = (uint16_t)((val + mspacman_til_patch[idx]) & 0xFFFF);
            idx++;
        }
    }
    return (unsigned short (*)[8])out;
}

static unsigned long (*build_spritemap(const uint8_t *msz, size_t msz_sz))[64][16] {
    RomScratch g5e(0x1000); if (!g5e) return nullptr;
    RomScratch g5f(0x1000); if (!g5f) return nullptr;
    size_t got = 0;
    if (!zip_extract_public(msz, msz_sz, "5e", g5e, 0x1000, &got) || got < 0x1000) {
        Serial.printf("[mspacman] 5e (sprites) failed (got=%u)\n", (unsigned)got); return nullptr;
    }
    if (!zip_extract_public(msz, msz_sz, "5f", g5f, 0x1000, &got) || got < 0x1000) {
        Serial.printf("[mspacman] 5f (sprites) failed (got=%u)\n", (unsigned)got); return nullptr;
    }
    unsigned long *out = (unsigned long *)malloc(4 * 64 * 16 * sizeof(unsigned long));
    if (!out) return nullptr;
    (void)g5e;
    const int sprite_offset = 2048;
    const int max_gfx = 0x1000;
    int idx = 0;
    for (int flip = 0; flip < 4; flip++) {
        for (int s = 0; s < 64; s++) {
            for (int r = 0; r < 16; r++) {
                int base = sprite_offset + s * 64 + r;
                uint8_t b0l = (base       < max_gfx) ? g5f[base + 0]  : 0;
                uint8_t b0h = (base + 16  < max_gfx) ? g5f[base + 16] : 0;
                uint8_t b1l = (base + 32  < max_gfx) ? g5f[base + 32] : 0;
                uint8_t b1h = (base + 48  < max_gfx) ? g5f[base + 48] : 0;
                uint32_t wl = 0, wh = 0;
                for (int bit = 0; bit < 8; bit++) {
                    int p0l = (b0l >> (7 - bit)) & 1;
                    int p1l = (b1l >> (7 - bit)) & 1;
                    int p0h = (b0h >> (7 - bit)) & 1;
                    int p1h = (b1h >> (7 - bit)) & 1;
                    wl |= (uint32_t)(((p1l << 1) | p0l) << (bit * 2));
                    wh |= (uint32_t)(((p1h << 1) | p0h) << (bit * 2));
                }
                uint32_t val = (wh << 16) | wl;
                if (flip & 1) {
                    uint32_t nv = 0;
                    for (int c = 0; c < 16; c++) {
                        uint32_t px = (val >> (c * 2)) & 3;
                        nv |= (px << ((15 - c) * 2));
                    }
                    val = nv;
                }
                out[idx] = (val + mspacman_spr_patch[idx]) & 0xFFFFFFFFu;
                idx++;
            }
        }
    }
    return (unsigned long (*)[64][16])out;
}

extern "C" bool runtime_load_mspacman(void) {
    const char *path = "/roms/mspacman.zip";
    if (!SD.exists(path)) { Serial.println("[mspacman] mspacman.zip not on SD"); return false; }
    uint8_t *msz = nullptr; size_t msz_sz = 0;
    if (!sd_slurp(path, &msz, &msz_sz)) { Serial.println("[mspacman] slurp failed"); return false; }
    Serial.printf("[mspacman] loaded %s (%u bytes) - decoding...\n", path, (unsigned)msz_sz);

    bool ok = load_base_pacman_rom(msz, msz_sz);
    if (ok) ok = build_pacrom_auxrom(msz, msz_sz);
    if (ok) {
        unsigned short (*tm)[8]      = build_tilemap(msz, msz_sz);
        unsigned long  (*sp)[64][16] = build_spritemap(msz, msz_sz);
        if (tm && sp) { pacman_5e = tm; pacman_sprites = sp; }
        else          { ok = false; }
    }
    if (ok) {
        unsigned short *cm = build_colormap(msz, msz_sz);
        /* Wavetable: mspacman.zip's OWN 82s126.1m/3m PROMs — these ship
         * with 16 populated waves (Ms. Pac-Man's aux ROM sound engine
         * uses wave indices well past what plain Pac-Man does, e.g. 12/15).
         * Puckman.zip's PROMs zero-fill waves 9-15 because Pac-Man never
         * indexes there. */
        signed char *wt = build_wavetable(msz, msz_sz);
        if (cm && wt) {
            pacman_colormap  = (unsigned short (*)[4])cm;
            pacman_wavetable = (signed char    (*)[32])wt;
        } else {
            ok = false;
        }
    }
    free(msz);
    mspacman_decode = 0;
    Serial.printf("[mspacman] runtime load %s\n", ok ? "OK" : "FAILED");
    return ok;
}
