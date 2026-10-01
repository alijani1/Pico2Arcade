/* pico_romcheck.cpp — see pico_romcheck.h.
 *
 * Expected files + CRC32s of the supported MAME ROM sets (one set per game),
 * taken from known-good zips. Only the files the loaders actually read are
 * listed (e.g. galaga's unused prom-2.5c isn't). Checked against the CRC32s
 * stored in the zip's local file headers — fast (no decompression), and it
 * catches missing files and wrong ROM revisions / clone sets.
 */
#include <Arduino.h>
#include <string.h>
#include <strings.h>
#include "pico_romfs.h"
#include "pico_romcheck.h"

struct RomFile { const char *name; uint32_t crc; };

static const RomFile puckman_files[] = {
    { "pm1-1.7f", 0x2fc650bdu },
    { "pm1-2.3m", 0x77245b66u },
    { "pm1-3.1m", 0xa9cc86bfu },
    { "pm1-4.4a", 0x3eb3a8e4u },
    { "pm1_chg1.5e", 0x2066a0b7u },
    { "pm1_chg2.5h", 0x3591b89du },
    { "pm1_chg3.5f", 0x9e39323au },
    { "pm1_chg4.5j", 0x1b1d9096u },
    { "pm1_prg1.6e", 0xf36e88abu },
    { "pm1_prg2.6k", 0x618bd9b3u },
    { "pm1_prg3.6f", 0x7d177853u },
    { "pm1_prg4.6m", 0xd3e8914cu },
    { "pm1_prg5.6h", 0x6bf4f625u },
    { "pm1_prg6.6n", 0xa948ce83u },
    { "pm1_prg7.6j", 0xb6289b26u },
    { "pm1_prg8.6p", 0x17a88c13u },
};
static const RomFile mspacman_files[] = {
    { "82s123.7f", 0x2fc650bdu },
    { "82s126.1m", 0xa9cc86bfu },
    { "82s126.3m", 0x77245b66u },
    { "82s126.4a", 0x3eb3a8e4u },
    { "5f", 0x615af909u },
    { "5e", 0x5c281d01u },
    { "pacman.6e", 0xc1e6ab10u },
    { "pacman.6f", 0x1a6fb2d4u },
    { "pacman.6h", 0xbcdd1bebu },
    { "pacman.6j", 0x817d94e3u },
    { "u5", 0xf45fbbcdu },
    { "u6", 0xa90e7000u },
    { "u7", 0xc82cd714u },
};
static const RomFile galaga_files[] = {
    { "gg1_10.4f", 0xdd6f1afcu },
    { "gg1_11.4d", 0xad447c80u },
    { "gg1_1b.3p", 0xab036c9fu },
    { "gg1_2b.3m", 0xd9232240u },
    { "gg1_3.2m", 0x753ce503u },
    { "gg1_4b.2l", 0x499fcc76u },
    { "gg1_5b.3f", 0xbb5caae3u },
    { "gg1_7b.2c", 0xd016686bu },
    { "gg1_9.4l", 0x58b2f47cu },
    { "prom-1.1d", 0x7a2815b4u },
    { "prom-3.1c", 0x4a04bb6bu },
    { "prom-4.2n", 0x59b6edabu },
    { "prom-5.5n", 0x54603c6bu },
};
static const RomFile frogger_files[] = {
    { "frogger.26", 0x597696d6u },
    { "frogger.27", 0xb6e6fcc3u },
    { "frogger.606", 0xf524ee30u },
    { "frogger.607", 0x05f7d883u },
    { "frogger.608", 0xe8ab0256u },
    { "frogger.609", 0x7380a48fu },
    { "frogger.610", 0x31d7eb27u },
    { "frsm3.7", 0xaca22ae0u },
    { "pr-91.6l", 0x413703bfu },
};
static const RomFile dkong_files[] = {
    { "c-2j.bpr", 0xd6412358u },
    { "c-2k.bpr", 0xe273ede5u },
    { "c_5at_g.bin", 0xb9005ac0u },
    { "c_5bt_g.bin", 0x1c97d324u },
    { "c_5ct_g.bin", 0x5ec461ecu },
    { "c_5et_g.bin", 0xba70b88bu },
    { "l_4m_b.bin", 0x59f8054du },
    { "l_4n_b.bin", 0x672e4714u },
    { "l_4r_b.bin", 0xfeaa59eeu },
    { "l_4s_b.bin", 0x20f2ef7eu },
    { "s_3i_b.bin", 0x45a4ed06u },
    { "s_3j_b.bin", 0x4743fe92u },
    { "v-5e.bpr", 0xb869b8f5u },
    { "v_3pt.bin", 0x15e9c5e9u },
    { "v_5h_b.bin", 0x12c8c95du },
};
static const RomFile digdug_files[] = {
    { "136007.109", 0x77245b66u },
    { "136007.110", 0x7a2815b4u },
    { "136007.111", 0x00c7c419u },
    { "136007.112", 0xe9b3e08eu },
    { "136007.113", 0x4cb9da99u },
    { "dd1.10b", 0x2cf399c2u },
    { "dd1.11", 0x7b383983u },
    { "dd1.12", 0xc58252a0u },
    { "dd1.13", 0x458499e9u },
    { "dd1.14", 0x2829ec99u },
    { "dd1.15", 0xe22957c8u },
    { "dd1.7", 0xa41bce72u },
    { "dd1.9", 0xf14a6fe1u },
    { "dd1a.1", 0xa80ec984u },
    { "dd1a.2", 0x559f00bdu },
    { "dd1a.3", 0x8cbc6fe1u },
    { "dd1a.4", 0xd066f830u },
    { "dd1a.5", 0x6687933bu },
    { "dd1a.6", 0x843d857fu },
};

static const RomFile invaders_files[] = {
    { "invaders.e", 0x14e538b0u },
    { "invaders.f", 0x0ccead96u },
    { "invaders.g", 0x6bfaca4au },
    { "invaders.h", 0x734f5ad8u },
};

static const RomFile scobra_files[] = {
    { "2e", 0x8e7245cdu },
    { "2h", 0x7244f21cu },
    { "2l", 0xd52affdeu },
    { "5c", 0xd4346959u },
    { "5d", 0xcc025d95u },
    { "5e", 0x1628c53fu },
    { "82s123.6e", 0x9b87f90du },
    { "epr1265.2c", 0xa0744b3fu },
    { "epr1267.2f", 0x47a4e6fbu },
    { "epr1269.2j", 0xe1f8a801u },
    { "epr1273.5f", 0xa96316d3u },
    { "epr1274.5h", 0x64d113b4u },
};

static const RomFile scramble_files[] = {
    { "c01s.6e", 0x4e3caeabu },
    { "c1.5h", 0x11fd2887u },
    { "c2.5f", 0x4708845bu },
    { "ot1.5c", 0xbcd297f0u },
    { "ot2.5d", 0xde7912dau },
    { "ot3.5e", 0xba2fa933u },
    { "s1.2d", 0xea35ccaau },
    { "s2.2e", 0xe7bba1b3u },
    { "s3.2f", 0x12d7fc3eu },
    { "s4.2h", 0xb59360ebu },
    { "s5.2j", 0x4919a91cu },
    { "s6.2l", 0x26a4547bu },
    { "s7.2m", 0x0bb49470u },
    { "s8.2p", 0x6a5740e5u },
};

static const RomFile dkongjr_files[] = {    /* MAME dkongjr (US, parent) */
    { "djr1-c-2e.2e", 0x463dc7adu },
    { "djr1-c-2f.2f", 0x47ba0042u },
    { "djr1-c_3h.3h", 0x715da5f8u },
    { "djr1-c_5b_f-2.5b", 0xdea28158u },
    { "djr1-c_5c_f-2.5c", 0x6fb5faf6u },
    { "djr1-c_5e_f-2.5e", 0xd042b6a8u },
    { "djr1-v-2n.2n", 0xdbf185bfu },
    { "djr1-v.3n", 0x8d51aca9u },
    { "djr1-v.3p", 0x4ef64ba5u },
    { "djr1-v_7c.7c", 0xdc7f4164u },
    { "djr1-v_7d.7d", 0x0ce7dcf6u },
    { "djr1-v_7e.7e", 0x24d1ff17u },
    { "djr1-v_7f.7f", 0x0f8c083fu },
};
static const RomFile dkongjrj_files[] = {   /* MAME dkongjrj (Japan) */
    { "c-2e.bpr", 0x463dc7adu },
    { "c-2f.bpr", 0x47ba0042u },
    { "c_3h.bin", 0x715da5f8u },
    { "c_5ba.bin", 0x50a015ceu },
    { "c_5ca.bin", 0xc0a18f0du },
    { "c_5ea.bin", 0xa81dd00cu },
    { "v-2n.bpr", 0xdbf185bfu },
    { "v_3na.bin", 0xa95c4c63u },
    { "v_3pa.bin", 0x4974ffefu },
    { "v_7c.bin", 0xdc7f4164u },
    { "v_7d.bin", 0x0ce7dcf6u },
    { "v_7e.bin", 0x24d1ff17u },
    { "v_7f.bin", 0x0f8c083fu },
};

static const RomFile pooyan_files[] = {
    { "1.4a", 0xbb319c63u },
    { "2.5a", 0xa1463d98u },
    { "3.6a", 0xfe1a9e08u },
    { "4.7a", 0x9e0f9bccu },
    { "5.8a", 0x1097c2b6u },
    { "6.9a", 0xb2d8c121u },
    { "7.9g", 0xbbe6d6e4u },
    { "8.10g", 0x931b29ebu },
    { "pooyan.pr1", 0xa06a6d0eu },
    { "pooyan.pr2", 0x82748c0bu },
    { "pooyan.pr3", 0x8cd4cd60u },
    { "xx.7a", 0xfbe2b368u },
    { "xx.8a", 0xe1795b3du },
};

struct RomSet { const char *zip; const RomFile *files; int n; };
#define SET(z) { "/roms/" #z ".zip", z##_files, (int)(sizeof(z##_files) / sizeof(z##_files[0])) }
static const RomSet SETS[] = {
    SET(puckman), SET(mspacman), SET(galaga), SET(frogger), SET(dkong), SET(digdug), SET(invaders), SET(scobra), SET(scramble), SET(dkongjr), SET(dkongjrj), SET(pooyan),
};

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

extern "C" int romcheck_crcs_for(const char *name, uint32_t *crcs, int max) {
    int n = 0;
    for (const RomSet &s : SETS)
        for (int i = 0; i < s.n && n < max; i++)
            if (strcmp(s.files[i].name, name) == 0) crcs[n++] = s.files[i].crc;
    return n;
}

/* Is there an entry with this CRC32 in the zip (under any name)? */
static bool zip_has_crc(const uint8_t *zip, size_t size, uint32_t crc) {
    const uint8_t *p = zip, *end = zip + size;
    while (p + 30 <= end && rd32(p) == 0x04034b50u) {
        if (rd32(p + 14) == crc) return true;
        p += 30 + rd16(p + 26) + rd16(p + 28) + rd32(p + 18);
    }
    return false;
}

/* CRC32 of `name` from the zip's local headers (the same walk the loader's
 * zip_extract() does). False if not found. */
static bool zip_crc(const uint8_t *zip, size_t size, const char *name, uint32_t *crc) {
    const uint8_t *p = zip, *end = zip + size;
    size_t want = strlen(name);
    while (p + 30 <= end && rd32(p) == 0x04034b50u) {
        uint32_t csize = rd32(p + 18);
        uint16_t nlen = rd16(p + 26), elen = rd16(p + 28);
        if (nlen == want && strncmp((const char *)(p + 30), name, want) == 0) {
            *crc = rd32(p + 14);
            return true;
        }
        p += 30 + nlen + elen + csize;
    }
    return false;
}

extern "C" bool romcheck_zip(const char *zipPath, char *why, size_t whyLen) {
    const RomSet *set = nullptr;
    for (const RomSet &s : SETS) if (strcmp(s.zip, zipPath) == 0) set = &s;
    if (!set) return true;                           /* no table: not checked */
    uint8_t *zip = nullptr;
    size_t size = 0;
    if (!romfs_slurp(zipPath, &zip, &size)) { snprintf(why, whyLen, "can't read the zip"); return false; }
    bool ok = true;
    int missing = 0, wrong = 0;
    const char *first = nullptr;
    for (int i = 0; i < set->n; i++) {
        uint32_t crc;
        bool named = zip_crc(zip, size, set->files[i].name, &crc);
        if (named && crc == set->files[i].crc) continue;
        if (zip_has_crc(zip, size, set->files[i].crc)) continue;     /* same ROM, other name */
        if (!first) first = set->files[i].name;
        if (named) wrong++; else missing++;
        ok = false;
    }
    free(zip);
    if (!ok) {
        if (missing && wrong) snprintf(why, whyLen, "%d missing, %d wrong (%s)", missing, wrong, first);
        else if (missing)     snprintf(why, whyLen, "%d file%s missing (%s)", missing, missing > 1 ? "s" : "", first);
        else                  snprintf(why, whyLen, "%d wrong file%s (%s)", wrong, wrong > 1 ? "s" : "", first);
        Serial.printf("[romcheck] %s: %s\n", zipPath, why);
    }
    return ok;
}
