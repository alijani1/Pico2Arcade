/* pico_menu.cpp — Pico2Arcade boot-time game picker.
 *
 * A simple text list of playable games (only those with runtime loaders
 * wired). User navigates with the joystick / BTN_A and picks one. We
 * load that game's data from SD, set the `machine` variable, and
 * return. Galagino then boots straight into the chosen game (its
 * built-in menu is skipped because `machine != MCH_MENU`).
 */

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "pico_romfs.h"   /* ROM zips: SD card or the Quad ROM drive */
#include "config.h"        /* ENABLE_PACMAN / ENABLE_GALAGA — must come first */
#include "emulation.h"     /* machine variable + MCH_* constants */

extern "C" void audio_dkong_bitrate(char is_dkong);
extern "C" void pico_audio_amp_on(void);
extern "C" void pico_audio_set_volume_pct(int pct);
extern "C" int  pico_audio_get_volume_pct(void);
extern "C" bool pico_input_menu_tapped(void);
#include "pico_handoff.h"
#include "pico_bench.h"
#include "pico_board.h"
#include "pico_drive.h"
#include "pico_romcheck.h"
#include "pico_hiscore.h"

extern "C" bool runtime_load_pacman(void);
extern "C" bool runtime_load_mspacman(void);
extern "C" bool runtime_load_galaga(void);
extern "C" bool runtime_load_frogger(void);
extern "C" bool runtime_load_dkong(void);
extern "C" bool runtime_load_dkongjr(void);
extern "C" bool runtime_load_dkongjrj(void);
extern "C" bool runtime_load_digdug(void);
extern "C" bool runtime_load_invaders(void);
extern "C" bool runtime_load_scobra(void);
extern "C" bool runtime_load_pooyan(void);
extern "C" bool runtime_load_timeplt(void);
extern "C" bool runtime_load_xevious(void);
extern "C" bool runtime_load_scramble(void);
extern "C" unsigned char buttons_get(void);
/* Gate that setup1() polls before starting the emulator on core 1.
 * Defined in pico_core1.cpp. */
extern volatile int g_menu_done;

/* Machine constants only exist in multi-machine mode. In SINGLE_MACHINE
 * we skip the assignment (the compiled-in game just runs). */
#ifdef SINGLE_MACHINE
#  define MCH_UNUSED 0
#  define SET_MACHINE(m)   /* nothing */
#else
#  define SET_MACHINE(m)   do { machine = (m); } while(0)
#endif

struct GameEntry {
    const char *label;
    const char *zip;     /* ROM set on SD; game is listed only if present */
    signed char mch;
    bool (*loader)(void);
    bool two_fire;       /* separate A / B fire buttons (pico_input.cpp) */
    unsigned char variant;   /* ROM set variant of the same machine (own high-score slot) */
};
extern "C" unsigned char pico_hiscore_variant;
extern "C" volatile int g_two_fire;

static const GameEntry games[] = {
#ifdef ENABLE_PACMAN
    { "Pac-Man", "/roms/puckman.zip",
#  ifdef SINGLE_MACHINE
      0,
#  else
      MCH_PACMAN,
#  endif
      runtime_load_pacman },
#endif
#ifdef ENABLE_MSPACMAN
    { "Ms. Pac-Man", "/roms/mspacman.zip",
#  ifdef SINGLE_MACHINE
      0,
#  else
      MCH_MSPACMAN,
#  endif
      runtime_load_mspacman },
#endif
#ifdef ENABLE_GALAGA
    { "Galaga", "/roms/galaga.zip",
#  ifdef SINGLE_MACHINE
      0,
#  else
      MCH_GALAGA,
#  endif
      runtime_load_galaga },
#endif
#ifdef ENABLE_FROGGER
    { "Frogger", "/roms/frogger.zip",
#  ifdef SINGLE_MACHINE
      0,
#  else
      MCH_FROGGER,
#  endif
      runtime_load_frogger },
#endif
#ifdef ENABLE_DKONG
    { "Donkey Kong", "/roms/dkong.zip",
#  ifdef SINGLE_MACHINE
      0,
#  else
      MCH_DKONG,
#  endif
      runtime_load_dkong },
#endif
#ifdef ENABLE_DIGDUG
    { "Dig Dug", "/roms/digdug.zip",
#  ifdef SINGLE_MACHINE
      0,
#  else
      MCH_DIGDUG,
#  endif
      runtime_load_digdug },
#endif
#ifdef ENABLE_INVADERS
    { "Space Invaders", "/roms/invaders.zip", MCH_INVADERS, runtime_load_invaders },
#endif
#ifdef ENABLE_DKONGJR
    /* MAME parent (US) set; the Japan set is listed if that's the zip there */
    { "Donkey Kong Jr.", "/roms/dkongjr.zip", MCH_DKONGJR, runtime_load_dkongjr },
    { "Donkey Kong Jr. (J)", "/roms/dkongjrj.zip", MCH_DKONGJR, runtime_load_dkongjrj, false, 1 },
#endif
#ifdef ENABLE_POOYAN
    { "Pooyan", "/roms/pooyan.zip", MCH_POOYAN, runtime_load_pooyan },
#endif
#ifdef ENABLE_TIMEPLT
    { "Time Pilot", "/roms/timeplt.zip", MCH_TIMEPLT, runtime_load_timeplt },
#endif
#ifdef ENABLE_XEVIOUS
    { "Xevious", "/roms/xevious.zip", MCH_XEVIOUS, runtime_load_xevious, true },   /* zapper / bomb */
#endif
#ifdef ENABLE_SCOBRA
    { "Super Cobra", "/roms/scobra.zip", MCH_SCOBRA, runtime_load_scobra, true },
#endif
#ifdef ENABLE_SCRAMBLE
    { "Scramble", "/roms/scramble.zip", MCH_SCRAMBLE, runtime_load_scramble, true },
#endif
    /* Add per game as its runtime loader lands. */
};
static const int NUM_GAMES = sizeof(games) / sizeof(games[0]);

#ifdef PICO_GALAGINO_LANDSCAPE
extern "C" volatile int g_landscape_mode;
extern "C" void pico_config_save_landscape(int on);
#endif
extern "C" volatile int g_show_fps;
extern "C" void pico_config_save_show_fps(int on);
extern "C" int  pico_config_volume(void);
extern "C" void pico_config_save_volume(int pct);

/* Games whose ROM zip is actually on the SD card (indices into games[]).
 * Scanned once when the menu opens. */
static int avail[NUM_GAMES > 0 ? NUM_GAMES : 1];
static int nAvail = 0;
/* Per games[] index: the zip is there but isn't the supported ROM set
 * (pico_romcheck.cpp) — listed in red, and picking it explains why. */
static bool badSet[NUM_GAMES > 0 ? NUM_GAMES : 1];
static char badWhy[NUM_GAMES > 0 ? NUM_GAMES : 1][40];

static void scan_roms(void) {
    nAvail = 0;
    for (int i = 0; i < NUM_GAMES; i++) {
        bool ok = romfs_exists(games[i].zip);
        Serial.printf("[menu] %-12s %s %s\n", games[i].label, games[i].zip,
                      ok ? "found" : "missing");
        if (ok) {
            badSet[i] = !romcheck_zip(games[i].zip, badWhy[i], sizeof(badWhy[i]));
            avail[nAvail++] = i;
        }
    }
}

/* Volume row: LEFT/RIGHT (or FIRE, wrapping) step it; kept by the host via
 * the handoff, and standalone in flash too. */
static const int VOLUME_STEP = 10;
static void set_volume(int v) {
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    pico_audio_set_volume_pct(v);
    handoff_volume_write(v);
    pico_config_save_volume(v);
}

/* Menu rows: available games, then (landscape builds) the display
 * toggle, the FPS-overlay toggle, (flash boards) ROM Drive, Volume, then
 * Exit. Row kinds are derived from the row index. */
enum RowKind { ROW_GAME, ROW_DISPLAY, ROW_FPS, ROW_DRIVE, ROW_VOLUME, ROW_EXIT };
/* Display (portrait/landscape) toggle: landscape builds. (On the Quad the
 * choice is kept by the Quad firmware via the handoff flags — pico_config.cpp.) */
#if defined(PICO_GALAGINO_LANDSCAPE)
#  define MENU_DISPLAY_TOGGLE 1
#endif
#ifdef MENU_DISPLAY_TOGGLE
static const int SETTING_ROWS_BASE = 2;   /* Display, Show FPS */
#else
static const int SETTING_ROWS_BASE = 1;   /* Show FPS */
#endif
/* Flash-drive boards: + "ROM Drive (USB)" (pico_drive.cpp). */
#ifdef PICO_USB_DRIVE
static const int SETTING_ROWS = SETTING_ROWS_BASE + 2;   /* + ROM Drive, Volume */
#else
static const int SETTING_ROWS = SETTING_ROWS_BASE + 1;   /* + Volume */
#endif
static int menu_rows(void) {
    return nAvail + SETTING_ROWS + 1;  /* + Exit */
}
static RowKind row_kind(int row) {
    if (row < nAvail) return ROW_GAME;
    row -= nAvail;
#ifdef MENU_DISPLAY_TOGGLE
    if (row == 0) return ROW_DISPLAY;
    row--;
#endif
    if (row == 0) return ROW_FPS;
#ifdef PICO_USB_DRIVE
    if (row == 1) return ROW_DRIVE;
    row--;
#endif
    if (row == 1) return ROW_VOLUME;
    return ROW_EXIT;
}

/* Exit = reboot. From the PicoCartReader ARCADE tile this lands back in
 * the CartReader (it owns the boot slot); standalone it restarts us. */
static void exit_arcade(void) {
    handoff_exit_to_cartreader();
}

/* Volume indicator, top-right of the list (mirrors PicoCartReader's). */
static void draw_volume(TFT_eSPI &tft) {
    int v = pico_audio_get_volume_pct();
    uint16_t col = (v <= 0) ? TFT_YELLOW : TFT_LIGHTGREY;
    int x = tft.width() - 54, y = 12;
    tft.fillRect(x, y - 1, 50, 10, TFT_BLACK);
    tft.fillRect(x, y + 2, 3, 4, col);
    tft.fillTriangle(x + 3, y + 4, x + 7, y, x + 7, y + 8, col);
    tft.setTextSize(1);
    tft.setTextColor(col, TFT_BLACK);
    tft.setCursor(x + 10, y);
    if (v <= 0) tft.print("MUTE");
    else        tft.printf("%d%%", v);
}

/* Rows never get tighter than this (size-2 text is 16 px tall). When they
 * don't all fit, the list scrolls: a window of rows that follows the
 * selection, with arrows at the right edge when there is more above/below. */
static const int MIN_STEP = 18;
static int s_first = 0;                        /* first row shown */

/* Menu layout from the panel's real size (240x320 portrait or 320x240
 * landscape): drawing past the bottom of a landscape panel wraps onto its
 * top rows. Recomputed on every full draw. */
static int s_top, s_step, s_shown;
static bool s_wide;

static void menu_layout(TFT_eSPI &tft) {
    const int H = tft.height();
    s_wide = tft.width() > H;                  /* landscape: one hint line, list pulled up */
    const int HINT_H = s_wide ? 12 : 22;       /* hint lines at the bottom */
    /* A note line under the title only when there is something to say (no
     * ROMs / ARCADE.INI problem) — otherwise the list starts right below
     * the title, so the rows get more spacing. */
    const bool note = (nAvail == 0) || board_pins_warning()[0];
    int top = note ? 60 : (s_wide ? 28 : 40);
    int step = (H - HINT_H - top) / menu_rows();
    if (note && step < 20) {                   /* tight (landscape): pull the list up */
        top = 46;
        step = (H - HINT_H - top) / menu_rows();
    }
    if (step > 24) step = 24;
    int shown = menu_rows();
    if (step < MIN_STEP) {
        step = MIN_STEP;
        shown = (H - HINT_H - top) / step;
    }
    s_top = top; s_step = step; s_shown = shown;
}

/* Scroll the window so `sel` is visible; true if it moved. */
static bool menu_follow(int sel) {
    const int rows = menu_rows(), old = s_first;
    if (sel < s_first) s_first = sel;
    if (sel >= s_first + s_shown) s_first = sel - s_shown + 1;
    if (s_first > rows - s_shown) s_first = rows - s_shown;
    if (s_first < 0) s_first = 0;
    return s_first != old;
}

/* One row, painted over whatever was there (its own background first, then
 * the text on that background), so moving the highlight never blanks the
 * screen. */
static void draw_row(TFT_eSPI &tft, int i, int sel) {
    if (i < s_first || i >= s_first + s_shown) return;
    const int W = tft.width();
    const int y = s_top + (i - s_first) * s_step;
    /* Game names in yellow, settings / actions in white. */
    uint16_t fg = (row_kind(i) == ROW_GAME) ? (badSet[avail[i]] ? TFT_RED : TFT_YELLOW) : TFT_WHITE;
    uint16_t bg = (i == sel) ? TFT_BLUE : TFT_BLACK;
    tft.fillRect(4, y - 1, W - 22, s_step - 1, bg);   /* stops short of the scroll arrows */
    tft.setTextSize(2);
    tft.setTextColor(fg, bg);
    char label[40];
    switch (row_kind(i)) {
      case ROW_GAME: snprintf(label, sizeof(label), "%s", games[avail[i]].label); break;
#ifdef MENU_DISPLAY_TOGGLE
      case ROW_DISPLAY:
        snprintf(label, sizeof(label), "Display: %s", g_landscape_mode ? "Landscape" : "Portrait");
        break;
#endif
      case ROW_FPS:
        snprintf(label, sizeof(label), "Show FPS: %s", g_show_fps ? "On" : "Off");
        break;
      case ROW_DRIVE: snprintf(label, sizeof(label), "ROM Drive (USB)"); break;
      case ROW_VOLUME: {
        int v = pico_audio_get_volume_pct();
        if (v <= 0) snprintf(label, sizeof(label), "Volume: < Mute >");
        else        snprintf(label, sizeof(label), "Volume: < %d%% >", v);
        break;
      }
      default: snprintf(label, sizeof(label), "Exit"); break;
    }
    /* crop to what fits (12 px per size-2 char), leaving room for the
     * scroll arrows — a longer label would wrap onto the next row */
    int fit = (W - 14 - 20) / 12;
    if (fit >= 0 && fit < (int)sizeof(label) && (int)strlen(label) > fit) label[fit] = 0;
    tft.setCursor(14, y);
    tft.print(label);
}

/* Scroll arrows at the right edge: more rows above / below. */
static void draw_arrows(TFT_eSPI &tft) {
    const int W = tft.width(), rows = menu_rows();
    const int yUp = s_top + 2, yDn = s_top + s_shown * s_step - 12;
    tft.fillRect(W - 18, yUp, 12, 8, TFT_BLACK);
    tft.fillRect(W - 18, yDn, 12, 8, TFT_BLACK);
    if (s_first > 0)
        tft.fillTriangle(W - 12, yUp, W - 17, yUp + 7, W - 7, yUp + 7, TFT_LIGHTGREY);
    if (s_first + s_shown < rows)
        tft.fillTriangle(W - 12, yDn + 7, W - 17, yDn, W - 7, yDn, TFT_LIGHTGREY);
}

/* Visible rows + arrows, in place (after a scroll). */
static void draw_rows(TFT_eSPI &tft, int sel) {
    for (int i = s_first; i < s_first + s_shown; i++) draw_row(tft, i, sel);
    draw_arrows(tft);
}

/* Move the highlight from `old` to `sel`: just those two rows, or every
 * visible row when the list scrolls — never a full-screen clear. */
static void menu_select(TFT_eSPI &tft, int old, int sel) {
    if (menu_follow(sel)) draw_rows(tft, sel);
    else { draw_row(tft, old, sel); draw_row(tft, sel, sel); }
}

/* The whole menu: on entry and after another screen (load error, ...). */
static void draw_menu(TFT_eSPI &tft, int sel) {
    const int W = tft.width(), H = tft.height();
    menu_layout(tft);
    menu_follow(sel);
    const int HINT_H = s_wide ? 12 : 22;

    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(10, 8);
    tft.print("Arcade Games");
    tft.setTextSize(1);
    tft.setCursor(10, 32);
    if (nAvail == 0) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
#ifdef PICO_BOARD_TABLE
        tft.print("No ROMs found on the ROM drive.");
        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        tft.setCursor(10, 44);
#ifdef PICO_USB_DRIVE
        tft.print("ROM Drive (USB): copy MAME zips");
#else
        tft.print("Mini-Games > Arcade ROM Drive: copy MAME zips");
#endif
#else
        tft.print("No ROMs found on SD card.");
        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        tft.setCursor(10, 44);
        tft.print("Copy MAME zips to /roms/ (e.g. galaga.zip)");
#endif
    } else if (board_pins_warning()[0]) {
        tft.setTextColor(TFT_RED, TFT_BLACK);       /* ARCADE.INI problem (standalone) */
        tft.print(board_pins_warning());
    }

    draw_rows(tft, sel);

    /* Hint lines kept <= 37 chars (222 px) so they fit portrait width;
     * landscape gets a single <= 51-char line (306 px). */
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    if (s_wide) {
        tft.setCursor(6, H - HINT_H + 2);
        tft.print("FIRE select  LT/RT volume  hold MENU in game: list");
        draw_volume(tft);
        return;
    }
    (void)W;
    tft.setCursor(6, H - HINT_H + 2);
    tft.print("UP/DN pick  FIRE select  MENU exit");
    tft.setCursor(6, H - HINT_H + 12);
    tft.print("LT/RT volume  in game hold MENU: list");
    draw_volume(tft);
}

/* Show a load error and wait for a button press (then back to menu). */
static void show_load_error(TFT_eSPI &tft, int g) {
    tft.setTextSize(2);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, 140);
    tft.print("LOAD FAILED");
    tft.setTextSize(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(10, 166);
    tft.printf("Check %s on %s", games[g].zip + 6, romfs_where());   /* skip "/roms/" */
    tft.setCursor(10, 180);
    tft.print("Press FIRE/START to go back");
    while (buttons_get() & (BUTTON_FIRE | BUTTON_START)) delay(20);
    while (!(buttons_get() & (BUTTON_FIRE | BUTTON_START))) delay(20);
}

/* The zip isn't the supported MAME set (pico_romcheck.cpp). */
static void show_bad_set(TFT_eSPI &tft, int g) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, 40);
    tft.print("WRONG ROM SET");
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 70);
    tft.printf("%s:", games[g].zip + 6);                  /* skip "/roms/" */
    tft.setCursor(10, 84);
    tft.print(badWhy[g]);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(10, 108);
    tft.print("Use the MAME parent set for this game");
    tft.setCursor(10, 120);
    tft.printf("(non-merged), copied to %s.", romfs_where());
    tft.setCursor(10, 144);
    tft.print("Press FIRE/START to go back");
    while (buttons_get() & (BUTTON_FIRE | BUTTON_START)) delay(20);
    while (!(buttons_get() & (BUTTON_FIRE | BUTTON_START))) delay(20);
}

extern "C" void pico_menu_pick_and_load(void) {
    Serial.printf("[menu] entering (NUM_GAMES=%d)\n", NUM_GAMES);
    scan_roms();

    /* Volume comes from the CartReader on launch; also re-published so a
     * later return to the CartReader keeps it even if unchanged here. */
    int hv = handoff_volume_read();
#ifdef PICO_STANDALONE
    /* Standalone: the handoff volume only lives across our own relaunches —
     * keep it in flash too, and start from there after a power-on. */
    if (hv >= 0) pico_config_save_volume(hv);
    else hv = pico_config_volume();
#endif
    pico_audio_set_volume_pct(hv >= 0 ? hv : 100);
    handoff_volume_write(pico_audio_get_volume_pct());
    Serial.printf("[menu] volume %d%% (%s)\n", pico_audio_get_volume_pct(),
                  hv >= 0 ? "from CartReader" : "default");

    TFT_eSPI tft;
    /* tft.init() already ran in Video::begin — safe to skip re-init. This
     * object starts out assuming 240x320; match the rotation Video::begin
     * gave the panel so width()/height() (and the layout) are right. */
#ifdef PICO_GALAGINO_LANDSCAPE
    tft.setRotation(g_landscape_mode ? 3 : 0);
#endif
    int sel = 0;
    int g = -1;   /* chosen index into games[] */

#ifdef Z80_BENCH
    /* Benchmark: no menu — run the next installed game in the suite. */
    {
        int bi = bench_game_index();
        tft.fillScreen(TFT_BLACK);
        tft.setTextSize(2);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.setCursor(10, 100);
#ifdef BENCH_GAME
        /* Single named game: run it once, then done. */
        int cand = -1;
        for (int i = 0; i < nAvail; i++)
            if (strstr(games[avail[i]].zip, BENCH_STR(BENCH_GAME))) cand = avail[i];
        if (bi >= 1 || cand < 0) {
            if (cand < 0) {
                tft.print("BENCH: " BENCH_STR(BENCH_GAME) " not on SD");
                Serial.println("[BENCH] " BENCH_STR(BENCH_GAME) " zip not on SD");
            } else {
                tft.print("BENCH DONE");
            }
            bench_all_done();
        }
#else
        if (bi >= nAvail || bi >= BENCH_MAX_GAMES) {
            tft.print("BENCH DONE");
            bench_all_done();
        }
        int cand = avail[bi];
#endif
        tft.printf("BENCH %s", games[cand].label);
        if (!games[cand].loader()) {
            Serial.printf("[BENCH] load failed: %s\n", games[cand].label);
            bench_begin(games[cand].label, bi);   /* keeps index bookkeeping */
            bench_all_done();
        }
        bench_begin(games[cand].label, bi);
        g = cand;
    }
#endif

    while (g < 0) {
        unsigned char prev_mask = buttons_get();
        draw_menu(tft, sel);
        int vol_shown = pico_audio_get_volume_pct();

        while (true) {
            unsigned char m = buttons_get();
            if (pico_input_menu_tapped()) exit_arcade();
            if (pico_audio_get_volume_pct() != vol_shown) {
                vol_shown = pico_audio_get_volume_pct();
                draw_volume(tft);
            }
            /* UP / DOWN: a press moves one row (wrapping at the ends); held,
             * it repeats after 400 ms every 120 ms, stopping at the first /
             * last row so it doesn't race round the list. */
            {
                static unsigned long repeat_at = 0;
                const unsigned char dirs = BUTTON_UP | BUTTON_DOWN;
                unsigned char d = m & dirs;
                int step = 0;
                bool fresh = d && !(prev_mask & d);
                if (fresh) repeat_at = millis() + 400;
                else if (d && (long)(millis() - repeat_at) >= 0) repeat_at = millis() + 120;
                else d = 0;
                if (d & BUTTON_UP)   step = -1;
                if (d & BUTTON_DOWN) step = +1;
                if (step) {
                    int n = sel + step;
                    if (fresh) n = (n + menu_rows()) % menu_rows();
                    else if (n < 0 || n >= menu_rows()) n = sel;
                    if (n != sel) { int old = sel; sel = n; menu_select(tft, old, sel); }
                }
            }
            /* Volume row: LEFT/RIGHT step it in place. */
            if (row_kind(sel) == ROW_VOLUME) {
                int dv = ((m & BUTTON_RIGHT) && !(prev_mask & BUTTON_RIGHT)) ? VOLUME_STEP
                       : ((m & BUTTON_LEFT) && !(prev_mask & BUTTON_LEFT)) ? -VOLUME_STEP : 0;
                if (dv) {
                    set_volume(pico_audio_get_volume_pct() + dv);
                    vol_shown = pico_audio_get_volume_pct();
                    draw_row(tft, sel, sel);
                    draw_volume(tft);
                }
            }
            if ((m & (BUTTON_FIRE | BUTTON_START)) && !(prev_mask & (BUTTON_FIRE | BUTTON_START)))
                break;
            prev_mask = m;
            delay(20);
        }

        switch (row_kind(sel)) {
#ifdef MENU_DISPLAY_TOGGLE
          case ROW_DISPLAY:
            pico_config_save_landscape(g_landscape_mode ? 0 : 1);
            delay(300);
            /* Relaunch (not a plain reboot) so that under the CartReader
             * we come back to this menu instead of the CartReader. */
            handoff_relaunch_arcade();
            break;
#endif
          case ROW_FPS:
            pico_config_save_show_fps(!g_show_fps);
            break;             /* loop redraws the menu with the new value */
#ifdef PICO_USB_DRIVE
          case ROW_DRIVE:
            pico_drive_request();          /* reboots into ROM drive mode */
            break;
#endif
          case ROW_VOLUME: {    /* FIRE: step up, wrapping to mute after 100% */
            int v = pico_audio_get_volume_pct();
            set_volume(v >= 100 ? 0 : v + VOLUME_STEP);
            break;
          }
          case ROW_EXIT:
            exit_arcade();
            break;
          default: {
            int cand = avail[sel];
            if (badSet[cand]) {           /* not the supported ROM set: say why */
                show_bad_set(tft, cand);
                break;
            }
            Serial.printf("[menu] picked: %s\n", games[cand].label);
            tft.fillScreen(TFT_BLACK);
            tft.setTextSize(2);
            tft.setTextColor(TFT_YELLOW, TFT_BLACK);
            tft.setCursor(10, 100);
            tft.printf("Loading %s...", games[cand].label);
            if (games[cand].loader()) g = cand;
            else show_load_error(tft, cand);
            break;
          }
        }
    }

    SET_MACHINE(games[g].mch);
    pico_hiscore_variant = games[g].variant;
    pico_hiscore_load();         /* saved high scores for this game (needs `machine`) */
#ifdef ENABLE_DKONG
    /* DKong's i8048 audio CPU produces 11765 Hz samples; other games
     * feed the DAC at 24000 Hz. Upstream's MCH_MENU handler switches
     * rate on game pick — since we bypass it, do it here or the ring
     * buffer starves and ticks. */
    audio_dkong_bitrate(games[g].mch == MCH_DKONG
#ifdef ENABLE_DKONGJR
                        || games[g].mch == MCH_DKONGJR
#endif
                        );
    g_two_fire = games[g].two_fire;
#endif
    tft.fillScreen(TFT_BLACK);
    /* Turn the PAM8302 amp ON now — during boot + menu the amp stays
     * disabled so the user doesn't hear DMA idle-buzz through the
     * speaker. */
    pico_audio_amp_on();
    /* Release the core-1 gate now that machine is set and the ROM
     * pointers are populated. */
    __dmb();          /* ensure machine + rom pointer writes are visible */
    g_menu_done = 1;
    Serial.println("[menu] game armed");
}
