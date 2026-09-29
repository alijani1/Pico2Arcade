/* pico_hiscore.cpp — see pico_hiscore.h.
 *
 * High-score locations: MAME plugins/hiscore/hiscore.dat (Leezer et al.,
 * based on highscore.mameworld.info). Each range is
 *   address, length, value expected in its first byte, value in its last byte
 * once the game has initialised its table; the table is restored only when
 * every range matches (same rule as MAME's plugin). Ranges include the
 * on-screen "HIGH SCORE" digits where the game keeps them in video RAM.
 * The same ranges (and the "pattern stable for 30 frames" rule below) are
 * used by GalaginoPlus (VirtualClaudioBoy), which saves high scores too.
 */
#include <Arduino.h>
#include <string.h>
#include "config.h"
#include "emulation.h"
#include "pico_board.h"
#include "pico_handoff.h"
#include "pico_hiscore.h"

#ifdef PICO_BOARD_TABLE
#include <hardware/flash.h>
#include <hardware/sync.h>
#include "pico_romfs.h"
#else
#include <SD.h>
#endif

extern volatile int g_menu_done;

struct HsRange { uint16_t addr; uint8_t len, first, last; };
struct HsGame  { signed char mch; uint16_t base; const HsRange *r; uint8_t n; };

/* Pac-Man / Ms. Pac-Man (same hardware): score, then the displayed digits. */
static const HsRange HS_PACMAN[]  = { {0x4e88, 0x04, 0x00, 0x00}, {0x43ed, 0x06, 0x40, 0x40}, {0x43d1, 0x01, 0x48, 0x48} };
static const HsRange HS_GALAGA[]  = { {0x8a20, 0x2d, 0x00, 0x18}, {0x83ed, 0x06, 0x00, 0x24} };
static const HsRange HS_FROGGER[] = { {0x83f1, 0x0a, 0x63, 0x01}, {0x83ef, 0x02, 0x63, 0x04} };
static const HsRange HS_DKONG[]   = { {0x6100, 0xaa, 0x94, 0x76}, {0x60b8, 0x03, 0x50, 0x00},
                                      {0x7641, 0x01, 0x00, 0x00}, {0x7621, 0x01, 0x00, 0x00},
                                      {0x7601, 0x01, 0x07, 0x07}, {0x75e1, 0x01, 0x06, 0x06},
                                      {0x75c1, 0x01, 0x05, 0x05}, {0x75a1, 0x01, 0x00, 0x00} };
static const HsRange HS_DIGDUG[]  = { {0x89a0, 0x25, 0x01, 0x01} };

#define HS(m, base, t) { m, base, t, (uint8_t)(sizeof(t) / sizeof(t[0])) }
/* Slot order = index in the save store; never reorder (saved data). */
static const HsGame GAMES[] = {
#ifdef ENABLE_PACMAN
    HS(MCH_PACMAN,   0x4000, HS_PACMAN),
#else
    { -1, 0, nullptr, 0 },
#endif
#ifdef ENABLE_MSPACMAN
    HS(MCH_MSPACMAN, 0x4000, HS_PACMAN),
#else
    { -1, 0, nullptr, 0 },
#endif
#ifdef ENABLE_GALAGA
    HS(MCH_GALAGA,   0x8000, HS_GALAGA),
#else
    { -1, 0, nullptr, 0 },
#endif
#ifdef ENABLE_FROGGER
    HS(MCH_FROGGER,  0x8000, HS_FROGGER),
#else
    { -1, 0, nullptr, 0 },
#endif
#ifdef ENABLE_DKONG
    HS(MCH_DKONG,    0x6000, HS_DKONG),
#else
    { -1, 0, nullptr, 0 },
#endif
#ifdef ENABLE_DIGDUG
    HS(MCH_DIGDUG,   0x8000, HS_DIGDUG),
#else
    { -1, 0, nullptr, 0 },
#endif
};
static const int NGAMES = sizeof(GAMES) / sizeof(GAMES[0]);

/* Saved store: a 256-byte header page + one 256-byte page per game
 * (7 x 256 = 1792 bytes, programmed as whole flash pages). */
static const uint32_t HS_MAGIC = 0x48495343u;          /* "CSIH" */
static const int SLOT_DATA = 252;
struct HsSlot  { uint8_t len, valid, pad[2]; uint8_t data[SLOT_DATA]; };
struct HsStore { uint32_t magic, version; uint8_t pad[248]; HsSlot slot[6]; };
static_assert(sizeof(HsStore) == 1792, "store must be whole 256-byte pages");
static_assert(sizeof(GAMES) / sizeof(GAMES[0]) == 6, "one slot per game");

static HsStore s_store;                                 /* core 0 owns it (load/save) */

#ifdef PICO_BOARD_TABLE
/* Flash sector: Quad — just below the ROM drive (the last 4 KB of the
 * arcade slot, which the arcade image must never reach); standalone —
 * right above the settings sector. */
#ifndef PICO_HISCORE_PHYS
#define PICO_HISCORE_PHYS 0x301000u
#endif
static uint32_t s_phys;
static const uint8_t *s_xip;
static void hs_locate(void) {
#ifdef PICO_QUAD
    const uint8_t *xip; uint32_t phys, size;
    romfs_location(&xip, &phys, &size);
    s_phys = phys - FLASH_SECTOR_SIZE;
    s_xip  = xip - FLASH_SECTOR_SIZE;
#else
    s_phys = PICO_HISCORE_PHYS;
    s_xip  = (const uint8_t *)(XIP_BASE + PICO_HISCORE_PHYS);
#endif
}
#endif

/* ── core 1 state ── */
enum { HS_OFF, HS_WAIT, HS_RUN };
static int      s_state = HS_OFF;
static int      s_game = -1;                            /* index into GAMES */
static uint32_t s_frames = 0;
static uint8_t  s_saved[SLOT_DATA], s_prev[SLOT_DATA];  /* last saved / last seen */
static uint8_t  s_len = 0;
static uint32_t s_stableFrames = 0;
static uint32_t s_initFrames = 0;                       /* consecutive frames the init pattern held */
/* core 1 -> core 0 */
static uint8_t           s_pending[SLOT_DATA];
static volatile bool     s_saveReq = false;
static volatile bool     s_exitReq = false;

static const uint32_t WAIT_FRAMES   = 60 * 60;          /* give up restoring after 60 s */
/* The init pattern must hold this many consecutive frames before the saved
 * table goes in — guards against transient matches while the game boots
 * (RAM tests, cleared RAM). */
static const uint32_t INIT_STABLE_FRAMES = 30;
static const uint32_t CHECK_EVERY   = 30;               /* compare twice a second */
static const uint32_t STABLE_FRAMES = 60 * 10;          /* unchanged for 10 s -> save */

static uint8_t table_len(const HsGame &g) {
    uint8_t n = 0;
    for (int i = 0; i < g.n; i++) n += g.r[i].len;
    return n;
}
static void table_read(const HsGame &g, uint8_t *out) {
    for (int i = 0; i < g.n; i++) {
        memcpy(out, memory + (g.r[i].addr - g.base), g.r[i].len);
        out += g.r[i].len;
    }
}
static void table_write(const HsGame &g, const uint8_t *in) {
    for (int i = 0; i < g.n; i++) {
        memcpy(memory + (g.r[i].addr - g.base), in, g.r[i].len);
        in += g.r[i].len;
    }
}
static bool table_ready(const HsGame &g) {
    for (int i = 0; i < g.n; i++) {
        const uint8_t *p = memory + (g.r[i].addr - g.base);
        if (p[0] != g.r[i].first || p[g.r[i].len - 1] != g.r[i].last) return false;
    }
    return true;
}

extern "C" void pico_hiscore_load(void) {
    memset(&s_store, 0, sizeof(s_store));
#ifdef PICO_BOARD_TABLE
    hs_locate();
    const HsStore *f = (const HsStore *)s_xip;
    if (f->magic == HS_MAGIC) memcpy(&s_store, f, sizeof(s_store));
#else
    File f = SD.open("/gal_hi.bin", FILE_READ);
    if (f) {
        if (f.read((uint8_t *)&s_store, sizeof(s_store)) != sizeof(s_store) || s_store.magic != HS_MAGIC)
            memset(&s_store, 0, sizeof(s_store));
        f.close();
    }
#endif
    s_game = -1;
    for (int i = 0; i < NGAMES; i++) if (GAMES[i].mch == machine && GAMES[i].n) s_game = i;
    if (s_game >= 0) {
        s_len = table_len(GAMES[s_game]);
        s_initFrames = 0;
        s_state = HS_WAIT;
        const HsSlot &sl = s_store.slot[s_game];
        Serial.printf("[hiscore] slot %d: %s\n", s_game,
                      (sl.valid && sl.len == s_len) ? "saved table" : "none yet");
    }
}

extern "C" void pico_hiscore_frame(void) {
#ifdef Z80_BENCH
    return;                                  /* benchmark runs must stay identical */
#endif
    if (s_state == HS_OFF || !g_menu_done || s_exitReq) return;
    const HsGame &g = GAMES[s_game];
    s_frames++;
    if (s_state == HS_WAIT) {
        if (!table_ready(g)) s_initFrames = 0;
        else if (++s_initFrames >= INIT_STABLE_FRAMES) {
            const HsSlot &sl = s_store.slot[s_game];
            if (sl.valid && sl.len == s_len) table_write(g, sl.data);
            table_read(g, s_saved);
            memcpy(s_prev, s_saved, s_len);
            s_stableFrames = 0;
            s_state = HS_RUN;
            return;
        }
        if (s_frames > WAIT_FRAMES)
            s_state = HS_OFF;                /* table never showed up: leave it alone */
        return;
    }
    if (s_frames % CHECK_EVERY) return;
    uint8_t cur[SLOT_DATA];
    table_read(g, cur);
    if (memcmp(cur, s_prev, s_len) != 0) {   /* still changing (a game in progress) */
        memcpy(s_prev, cur, s_len);
        s_stableFrames = 0;
        return;
    }
    s_stableFrames += CHECK_EVERY;
    if (s_stableFrames >= STABLE_FRAMES && memcmp(cur, s_saved, s_len) != 0 && !s_saveReq) {
        memcpy(s_pending, cur, s_len);
        memcpy(s_saved, cur, s_len);
        s_saveReq = true;                    /* core 0 writes it */
    }
}

static void store_write(void) {
    HsSlot &sl = s_store.slot[s_game];
    sl.len = s_len;
    sl.valid = 1;
    memcpy(sl.data, s_pending, s_len);
    s_store.magic = HS_MAGIC;
    s_store.version = 1;
#ifdef PICO_BOARD_TABLE
    rp2040.idleOtherCore();                  /* core 1 (emulator) runs from flash */
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(s_phys, FLASH_SECTOR_SIZE);
    flash_range_program(s_phys, (const uint8_t *)&s_store, sizeof(s_store));
    restore_interrupts(irq);
    rp2040.resumeOtherCore();
#else
    SD.remove("/gal_hi.bin");
    File f = SD.open("/gal_hi.bin", FILE_WRITE);
    if (f) { f.write((const uint8_t *)&s_store, sizeof(s_store)); f.close(); }
#endif
    Serial.printf("[hiscore] saved slot %d\n", s_game);
}

extern "C" void pico_hiscore_service(void) {
    if (s_saveReq) {
        store_write();
        s_saveReq = false;
    }
    if (s_exitReq) handoff_relaunch_arcade();
}

extern "C" void pico_hiscore_exit_to_list(void) {
    if (s_state == HS_RUN) {
        uint8_t cur[SLOT_DATA];
        table_read(GAMES[s_game], cur);
        if (memcmp(cur, s_saved, s_len) != 0 && !s_saveReq) {
            memcpy(s_pending, cur, s_len);
            memcpy(s_saved, cur, s_len);
            s_saveReq = true;
        }
    }
    if (s_saveReq) {
        if (get_core_num() == 0) {           /* called on core 0: save right here */
            pico_hiscore_service();
        } else {                             /* core 1: let core 0 save, then relaunch */
            s_exitReq = true;
            for (;;) tight_loop_contents();
        }
    }
    handoff_relaunch_arcade();
}
