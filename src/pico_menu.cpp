/* pico_menu.cpp — PicoGalagino boot-time game picker.
 *
 * A simple text list of playable games (only those with runtime loaders
 * wired). User navigates with the joystick / BTN_A and picks one. We
 * load that game's data from SD, set the `machine` variable, and
 * return. Galagino then boots straight into the chosen game (its
 * built-in menu is skipped because `machine != MCH_MENU`).
 */

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SD.h>
#include "config.h"        /* ENABLE_PACMAN / ENABLE_GALAGA — must come first */
#include "emulation.h"     /* machine variable + MCH_* constants */

extern "C" void audio_dkong_bitrate(char is_dkong);
extern "C" void pico_audio_amp_on(void);
extern "C" void pico_audio_set_volume_pct(int pct);
extern "C" int  pico_audio_get_volume_pct(void);
extern "C" bool pico_input_menu_tapped(void);
#include "pico_handoff.h"
#include "pico_bench.h"

extern "C" bool runtime_load_pacman(void);
extern "C" bool runtime_load_mspacman(void);
extern "C" bool runtime_load_galaga(void);
extern "C" bool runtime_load_frogger(void);
extern "C" bool runtime_load_dkong(void);
extern "C" bool runtime_load_digdug(void);
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
};

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
    /* Add per game as its runtime loader lands. */
};
static const int NUM_GAMES = sizeof(games) / sizeof(games[0]);

#ifdef PICO_GALAGINO_LANDSCAPE
extern "C" volatile int g_landscape_mode;
extern "C" void pico_config_save_landscape(int on);
#endif
extern "C" volatile int g_show_fps;
extern "C" void pico_config_save_show_fps(int on);

/* Games whose ROM zip is actually on the SD card (indices into games[]).
 * Scanned once when the menu opens. */
static int avail[NUM_GAMES > 0 ? NUM_GAMES : 1];
static int nAvail = 0;

static void scan_roms(void) {
    nAvail = 0;
    for (int i = 0; i < NUM_GAMES; i++) {
        bool ok = SD.exists(games[i].zip);
        Serial.printf("[menu] %-12s %s %s\n", games[i].label, games[i].zip,
                      ok ? "found" : "missing");
        if (ok) avail[nAvail++] = i;
    }
}

/* Menu rows: available games, then (landscape builds) the display
 * toggle, the FPS-overlay toggle, then Exit. Row kinds are derived from
 * the row index. */
enum RowKind { ROW_GAME, ROW_DISPLAY, ROW_FPS, ROW_EXIT };
#ifdef PICO_GALAGINO_LANDSCAPE
static const int SETTING_ROWS = 2;   /* Display, Show FPS */
#else
static const int SETTING_ROWS = 1;   /* Show FPS */
#endif
static int menu_rows(void) {
    return nAvail + SETTING_ROWS + 1;  /* + Exit */
}
static RowKind row_kind(int row) {
    if (row < nAvail) return ROW_GAME;
    row -= nAvail;
#ifdef PICO_GALAGINO_LANDSCAPE
    if (row == 0) return ROW_DISPLAY;
    row--;
#endif
    if (row == 0) return ROW_FPS;
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
    int x = 186, y = 12;
    tft.fillRect(x, y - 1, 50, 10, TFT_BLACK);
    tft.fillRect(x, y + 2, 3, 4, col);
    tft.fillTriangle(x + 3, y + 4, x + 7, y, x + 7, y + 8, col);
    tft.setTextSize(1);
    tft.setTextColor(col, TFT_BLACK);
    tft.setCursor(x + 10, y);
    if (v <= 0) tft.print("MUTE");
    else        tft.printf("%d%%", v);
}

/* Draw the menu list, highlighting the selected row. Uses the same
 * TFT_eSPI instance the video code owns — we don't have a reference to
 * it here, so we make our own; TFT_eSPI's setup is idempotent. */
static void draw_menu(TFT_eSPI &tft, int sel) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(10, 8);
    tft.print("Arcade Games");
    tft.setTextSize(1);
    tft.setCursor(10, 32);
    if (nAvail == 0) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.print("No ROMs found on SD card.");
        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        tft.setCursor(10, 44);
        tft.print("Copy MAME zips to /roms/ (e.g. galaga.zip)");
    } else {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.print("Select a game:");
    }

    tft.setTextSize(2);
    int y = 60;
    for (int i = 0; i < menu_rows(); i++) {
        if (i == sel) {
            tft.fillRect(4, y - 2, 230, 20, TFT_BLUE);
            tft.setTextColor(TFT_WHITE, TFT_BLUE);
        } else {
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        tft.setCursor(14, y);
        switch (row_kind(i)) {
          case ROW_GAME: tft.print(games[avail[i]].label); break;
#ifdef PICO_GALAGINO_LANDSCAPE
          case ROW_DISPLAY:
            tft.printf("Display: %s", g_landscape_mode ? "Landscape" : "Portrait");
            break;
#endif
          case ROW_FPS:
            tft.printf("Show FPS: %s", g_show_fps ? "On" : "Off");
            break;
          default: tft.print("Exit"); break;
        }
        y += 24;
    }

    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(10, 300);
    tft.print("UP/DOWN: pick  FIRE/START: select  MENU: exit");
    tft.setCursor(10, 310);
    tft.print("MENU+UP/DN: volume  In game: hold START+SELECT");
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
    tft.printf("Check %s on the SD card", games[g].zip);
    tft.setCursor(10, 180);
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
    pico_audio_set_volume_pct(hv >= 0 ? hv : 100);
    handoff_volume_write(pico_audio_get_volume_pct());
    Serial.printf("[menu] volume %d%% (%s)\n", pico_audio_get_volume_pct(),
                  hv >= 0 ? "from CartReader" : "default");

    TFT_eSPI tft;
    /* tft.init() already ran in Video::begin — safe to skip re-init. */
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
            /* Edge-detect direction/fire — react on press, not on hold. */
            if ((m & BUTTON_UP) && !(prev_mask & BUTTON_UP)) {
                if (--sel < 0) sel = menu_rows() - 1;
                draw_menu(tft, sel);
            }
            if ((m & BUTTON_DOWN) && !(prev_mask & BUTTON_DOWN)) {
                if (++sel >= menu_rows()) sel = 0;
                draw_menu(tft, sel);
            }
            if ((m & (BUTTON_FIRE | BUTTON_START)) && !(prev_mask & (BUTTON_FIRE | BUTTON_START)))
                break;
            prev_mask = m;
            delay(20);
        }

        switch (row_kind(sel)) {
#ifdef PICO_GALAGINO_LANDSCAPE
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
          case ROW_EXIT:
            exit_arcade();
            break;
          default: {
            int cand = avail[sel];
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
#ifdef ENABLE_DKONG
    /* DKong's i8048 audio CPU produces 11765 Hz samples; other games
     * feed the DAC at 24000 Hz. Upstream's MCH_MENU handler switches
     * rate on game pick — since we bypass it, do it here or the ring
     * buffer starves and ticks. */
    audio_dkong_bitrate(games[g].mch == MCH_DKONG);
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
