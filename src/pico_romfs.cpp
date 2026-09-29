/* pico_romfs.cpp — see pico_romfs.h. */
#include <Arduino.h>
#include <string.h>
#include "pico_romfs.h"

#ifndef PICO_QUAD
/* ── SD card backend (PicoCartReader) ─────────────────────────────────── */
#include <SD.h>

extern "C" bool pico_sd_init(void);

extern "C" bool romfs_init(void) { return pico_sd_init(); }

extern "C" bool romfs_exists(const char *path) { return SD.exists(path); }

extern "C" bool romfs_slurp(const char *path, uint8_t **out, size_t *out_sz) {
    File f = SD.open(path, FILE_READ);
    if (!f) { Serial.printf("[romfs] open failed: %s\n", path); return false; }
    size_t sz = f.size();
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { Serial.printf("[romfs] malloc %u bytes failed\n", (unsigned)sz); f.close(); return false; }
    size_t got = f.read(buf, sz);
    f.close();
    if (got != sz) { Serial.printf("[romfs] short read: %u/%u\n", (unsigned)got, (unsigned)sz); free(buf); return false; }
    *out = buf; *out_sz = sz;
    return true;
}

extern "C" const char *romfs_where(void) { return "/roms/ on the SD card"; }

#else
/* ── Quad flash ROM drive backend (read-only FAT12/FAT16 over XIP) ────── */
#include <hardware/structs/watchdog.h>
#include "pico_handoff.h"

#ifndef PICO_QUAD_FAT_WINDOW_OFS
#define PICO_QUAD_FAT_WINDOW_OFS 0x50000u    /* Quad: 0x3C0000 - 0x370000 */
#endif
#ifndef PICO_QUAD_FAT_SIZE
#define PICO_QUAD_FAT_SIZE       0x40000u    /* 256 KB */
#endif

static const uint8_t *vol = nullptr;         /* start of the FAT volume (XIP) */
static uint32_t volSize = 0;
static uint32_t bps, spc, fatOfs, rootOfs, rootEntries, dataOfs, clusterBytes, clusterCount;
static bool fat16 = false;

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Next cluster in the chain; >= 0xFF8 (FAT12) / 0xFFF8 (FAT16) = end. */
static uint32_t fat_next(uint32_t c) {
    if (fat16) return rd16(vol + fatOfs + c * 2);
    uint16_t v = rd16(vol + fatOfs + c + c / 2);
    return (c & 1) ? (v >> 4) : (v & 0x0FFF);
}
static bool fat_eoc(uint32_t c) { return fat16 ? c >= 0xFFF8 : c >= 0x0FF8; }
static bool cluster_ok(uint32_t c) { return c >= 2 && c < clusterCount + 2; }
static uint32_t cluster_ofs(uint32_t c) { return dataOfs + (c - 2) * clusterBytes; }

extern "C" bool romfs_init(void) {
    /* Where the Quad put the drive in our window (scratch[2]/[3]), else defaults. */
    uint32_t ofs = PICO_QUAD_FAT_WINDOW_OFS, size = PICO_QUAD_FAT_SIZE;
    uint32_t s2 = watchdog_hw->scratch[HANDOFF_SCRATCH_FATOFS];
    uint32_t s3 = watchdog_hw->scratch[HANDOFF_SCRATCH_FATSIZE];
    if ((s2 & 0xFFFF0000u) == HANDOFF_FATOFS_TAG)  ofs  = (s2 & 0xFFFFu) << 12;
    if ((s3 & 0xFFFF0000u) == HANDOFF_FATSIZE_TAG) size = (s3 & 0xFFFFu) << 12;
    vol = (const uint8_t *)(XIP_BASE + ofs);
    volSize = size;

    if (vol[510] != 0x55 || vol[511] != 0xAA) {
        Serial.printf("[romfs] no FAT volume at +0x%lx\n", (unsigned long)ofs);
        vol = nullptr; return false;
    }
    bps = rd16(vol + 11);
    spc = vol[13];
    uint32_t rsvd = rd16(vol + 14), nfats = vol[16], spf = rd16(vol + 22);
    rootEntries = rd16(vol + 17);
    uint32_t totSec = rd16(vol + 19);
    if (totSec == 0) totSec = rd32(vol + 32);
    if (bps != 512 || spc == 0 || nfats == 0 || spf == 0 || totSec * bps > volSize) {
        Serial.println("[romfs] unsupported FAT geometry");
        vol = nullptr; return false;
    }
    fatOfs = rsvd * bps;
    rootOfs = (rsvd + nfats * spf) * bps;
    uint32_t rootSec = (rootEntries * 32 + bps - 1) / bps;
    dataOfs = rootOfs + rootSec * bps;
    clusterBytes = spc * bps;
    clusterCount = (totSec * bps - dataOfs) / clusterBytes;
    fat16 = clusterCount >= 4085;
    Serial.printf("[romfs] FAT%d ROM drive at +0x%lx: %lu clusters x %lu B\n",
                  fat16 ? 16 : 12, (unsigned long)ofs,
                  (unsigned long)clusterCount, (unsigned long)clusterBytes);
    return true;
}

/* "galaga.zip" -> "GALAGA  ZIP" (8.3, space padded). False if not 8.3. */
static bool to_83(const char *name, char out[11]) {
    memset(out, ' ', 11);
    const char *dot = strrchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    if (base == 0 || base > 8) return false;
    for (size_t i = 0; i < base; i++) out[i] = toupper((unsigned char)name[i]);
    if (dot) {
        size_t ext = strlen(dot + 1);
        if (ext > 3) return false;
        for (size_t i = 0; i < ext; i++) out[8 + i] = toupper((unsigned char)dot[1 + i]);
    }
    return true;
}

/* Look for `name83` among `count` 32-byte entries at `dir`. */
static const uint8_t *scan_dir(const uint8_t *dir, uint32_t count, const char *name83, uint8_t wantAttr) {
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = dir + i * 32;
        if (e[0] == 0x00) return nullptr;                 /* end of directory */
        if (e[0] == 0xE5) continue;                        /* deleted */
        uint8_t attr = e[11];
        if (attr == 0x0F || (attr & 0x08)) continue;       /* LFN piece / volume label */
        if ((attr & 0x10) != wantAttr) continue;           /* file vs. directory */
        if (memcmp(e, name83, 11) == 0) return e;
    }
    return nullptr;
}

/* Find a directory entry in a subdirectory's cluster chain. */
static const uint8_t *scan_subdir(uint32_t cluster, const char *name83) {
    for (int guard = 0; cluster_ok(cluster) && guard < 4096; guard++) {
        const uint8_t *e = scan_dir(vol + cluster_ofs(cluster), clusterBytes / 32, name83, 0);
        if (e) return e;
        cluster = fat_next(cluster);
        if (fat_eoc(cluster)) break;
    }
    return nullptr;
}

/* Directory entry for the zip named by `path` (root, then ROMS folder). */
static const uint8_t *find_file(const char *path) {
    if (!vol) return nullptr;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    char n83[11];
    if (!to_83(name, n83)) return nullptr;
    const uint8_t *e = scan_dir(vol + rootOfs, rootEntries, n83, 0);
    if (e) return e;
    const char romsDir[11] = {'R','O','M','S',' ',' ',' ',' ',' ',' ',' '};
    const uint8_t *d = scan_dir(vol + rootOfs, rootEntries, romsDir, 0x10);
    return d ? scan_subdir(rd16(d + 26), n83) : nullptr;
}

extern "C" bool romfs_exists(const char *path) { return find_file(path) != nullptr; }

extern "C" bool romfs_slurp(const char *path, uint8_t **out, size_t *out_sz) {
    const uint8_t *e = find_file(path);
    if (!e) { Serial.printf("[romfs] not found: %s\n", path); return false; }
    uint32_t size = rd32(e + 28), cluster = rd16(e + 26);
    if (size == 0 || size > volSize) return false;
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) { Serial.printf("[romfs] malloc %lu bytes failed\n", (unsigned long)size); return false; }
    /* Copy cluster by cluster (the file may be fragmented). */
    uint32_t done = 0;
    while (done < size) {
        if (!cluster_ok(cluster)) { free(buf); Serial.println("[romfs] broken cluster chain"); return false; }
        uint32_t n = size - done;
        if (n > clusterBytes) n = clusterBytes;
        memcpy(buf + done, vol + cluster_ofs(cluster), n);
        done += n;
        if (done < size) cluster = fat_next(cluster);
    }
    *out = buf; *out_sz = size;
    return true;
}

extern "C" const char *romfs_where(void) { return "the QUAD ROMS drive"; }
#endif
