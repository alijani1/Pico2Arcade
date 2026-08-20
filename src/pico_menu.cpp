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
#include "config.h"        /* ENABLE_PACMAN / ENABLE_GALAGA — must come first */
#include "emulation.h"     /* machine variable + MCH_* constants */

extern "C" void audio_dkong_bitrate(char is_dkong);

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
    signed char mch;
    bool (*loader)(void);
};

static const GameEntry games[] = {
#ifdef ENABLE_PACMAN
    { "Pac-Man",
#  ifdef SINGLE_MACHINE
      0,
#  else
      MCH_PACMAN,
#  endif
      runtime_load_pacman },
#endif
#ifdef ENABLE_MSPACMAN
    { "Ms. Pac-Man",
#  ifdef SINGLE_MACHINE
      0,
#  else
      MCH_MSPACMAN,
#  endif
      runtime_load_mspacman },
#endif
#ifdef ENABLE_GALAGA
    { "Galaga",
#  ifdef SINGLE_MACHINE
      0,
#  else
      MCH_GALAGA,
#  endif
      runtime_load_galaga },
#endif
#ifdef ENABLE_FROGGER
    { "Frogger",
#  ifdef SINGLE_MACHINE
      0,
#  else
      MCH_FROGGER,
#  endif
      runtime_load_frogger },
#endif
#ifdef ENABLE_DKONG
    { "Donkey Kong",
#  ifdef SINGLE_MACHINE
      0,
#  else
      MCH_DKONG,
#  endif
      runtime_load_dkong },
#endif
#ifdef ENABLE_DIGDUG
    { "Dig Dug",
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
static const int MENU_TOTAL = NUM_GAMES + 1;
#else
static const int MENU_TOTAL = NUM_GAMES;
#endif

/* Draw the menu list, highlighting the selected row. Uses the same
 * TFT_eSPI instance the video code owns — we don't have a reference to
 * it here, so we make our own; TFT_eSPI's setup is idempotent. */
static void draw_menu(TFT_eSPI &tft, int sel) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(10, 8);
    tft.print("PicoGalagino");
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(10, 32);
    tft.print("Select a game:");

    tft.setTextSize(2);
    int y = 60;
    for (int i = 0; i < MENU_TOTAL; i++) {
        if (i == sel) {
            tft.fillRect(4, y - 2, 230, 20, TFT_BLUE);
            tft.setTextColor(TFT_WHITE, TFT_BLUE);
        } else {
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        tft.setCursor(14, y);
        if (i < NUM_GAMES) {
            tft.print(games[i].label);
        }
#ifdef PICO_GALAGINO_LANDSCAPE
        else {
            tft.printf("Display: %s", g_landscape_mode ? "Landscape" : "Portrait");
        }
#endif
        y += 24;
    }

    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(10, 300);
    tft.print("UP/DOWN: pick   FIRE/START: play");
}

extern "C" void pico_menu_pick_and_load(void) {
    Serial.printf("[menu] entering (NUM_GAMES=%d)\n", NUM_GAMES);
    if (NUM_GAMES == 0) {
        Serial.println("[menu] no games compiled in!");
        return;
    }
    if (NUM_GAMES == 1) {
        /* No choice — just load and go. */
        Serial.printf("[menu] only game: %s (auto-select)\n", games[0].label);
        games[0].loader();
        SET_MACHINE(games[0].mch);
#ifdef ENABLE_DKONG
        audio_dkong_bitrate(games[0].mch == MCH_DKONG);
#endif
        __dmb();
        g_menu_done = 1;
        return;
    }

    TFT_eSPI tft;
    /* tft.init() already ran in Video::begin — safe to skip re-init. */
    int sel = 0;
    unsigned char prev_mask = buttons_get();
    draw_menu(tft, sel);

    while (true) {
        unsigned char m = buttons_get();
        /* Edge-detect direction/fire — react on press, not on hold. */
        if ((m & BUTTON_UP) && !(prev_mask & BUTTON_UP)) {
            if (--sel < 0) sel = MENU_TOTAL - 1;
            draw_menu(tft, sel);
        }
        if ((m & BUTTON_DOWN) && !(prev_mask & BUTTON_DOWN)) {
            if (++sel >= MENU_TOTAL) sel = 0;
            draw_menu(tft, sel);
        }
        if ((m & (BUTTON_FIRE | BUTTON_START)) && !(prev_mask & (BUTTON_FIRE | BUTTON_START))) {
#ifdef PICO_GALAGINO_LANDSCAPE
            if (sel >= NUM_GAMES) {
                int new_mode = g_landscape_mode ? 0 : 1;
                pico_config_save_landscape(new_mode);
                delay(300);
                rp2040.reboot();
            }
#endif
            break;
        }
        prev_mask = m;
        delay(20);
    }

    /* Load the chosen game. */
    Serial.printf("[menu] picked: %s\n", games[sel].label);
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(10, 100);
    tft.printf("Loading %s...", games[sel].label);

    if (!games[sel].loader()) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.setCursor(10, 140);
        tft.print("LOAD FAILED");
        while (1) delay(1000);
    }
    SET_MACHINE(games[sel].mch);
#ifdef ENABLE_DKONG
    /* DKong's i8048 audio CPU produces 11765 Hz samples; other games
     * feed the DAC at 24000 Hz. Upstream's MCH_MENU handler switches
     * rate on game pick — since we bypass it, do it here or the ring
     * buffer starves and ticks. */
    audio_dkong_bitrate(games[sel].mch == MCH_DKONG);
#endif
    tft.fillScreen(TFT_BLACK);
    /* Release the core-1 gate now that machine is set and the ROM
     * pointers are populated. */
    __dmb();          /* ensure machine + rom pointer writes are visible */
    g_menu_done = 1;
    Serial.println("[menu] game armed");
}
