/* pico_board.cpp — see pico_board.h. */
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include "pico_board.h"

#ifdef PICO_BOARD_TABLE
#include "pico_romfs.h"

#ifdef PICO_QUAD
/* Quad Ultimate CX — pins match GameAdapterRGBPico data/quad.json: port 1
 * DB9 = the P1 nets 28-31 (fire 12, one button: A and B in two-button games);
 * the device buttons as there: Red (button 0) and Blue = A, Green (button 1)
 * and Yellow = B; the Joy-Con click also fires; Black = START, White = COIN
 * (the Quad's Start / Select), gray Menu; Joy-Con ADC on 44/45; PWM audio
 * 22 + PAM8302 enable 4; backlight MOSFET 23. */
board_pins_t g_pins = {
    /* up down left right */ 28, 29, 30, 31,
    /* fire fire2 fire3 fire4 */ 12, 14, 16, -1,
    /* fireb fireb2 fireb3 */ -1, 15, 17,
    /* start coin menu    */ 20, 19, 18,   /* Black = GPIO20 = START, White = GPIO19 = COIN */
    /* nav_up nav_down    */ 3, 2,          /* left / right side buttons */
    /* joy_x joy_y        */ 44, 45,
    /* joy_click          */ 21,
    /* audio amp          */ 22, 4,
    /* backlight          */ 23,
    /* stick_x stick_y    */ -1, -1,
};
#elif defined(PICO_5200)
/* Duo 5200 Ultimate — same device buttons, Joy-Con, audio and backlight as the
 * Quad (GameAdapterRGBPico data/duo5200.json), no DB9 joystick: directions come
 * from the Joy-Con and the port 1 Atari 5200 controller's analog stick (RC pots
 * on GPIO 40/41); A = its Fire A (GPIO 10), Red (button 0) or Blue, B = its
 * Fire B (GPIO 11), Green (button 1) or Yellow; the Joy-Con click also fires;
 * White = START, Black = COIN, gray Menu. */
board_pins_t g_pins = {
    /* up down left right */ -1, -1, -1, -1,
    /* fire fire2 fire3 fire4 */ 10, 14, 16, -1,
    /* fireb fireb2 fireb3 */ 11, 15, 17,
    /* start coin menu    */ 20, 19, 18,
    /* nav_up nav_down    */ -1, -1,
    /* joy_x joy_y        */ 44, 45,
    /* joy_click          */ 21,
    /* audio amp          */ 22, 4,
    /* backlight          */ 23,
    /* stick_x stick_y    */ 40, 41,
};
#else
/* Plain Pico 2 defaults (also the contents of the default ARCADE.INI). Chosen
 * clear of the display (SPI0: SCLK 6, MOSI 7, CS 5, DC 8, RST 9). */
board_pins_t g_pins = {
    /* up down left right */ 10, 11, 12, 13,
    /* fire fire2 fire3 fire4 */ 14, -1, -1, -1,
    /* fireb fireb2 fireb3 */ -1, -1, -1,
    /* start coin menu    */ 15, 16, 17,
    /* nav_up nav_down    */ -1, -1,
    /* joy_x joy_y        */ -1, -1,
    /* joy_click          */ -1,
    /* audio amp          */ 22, -1,
    /* backlight          */ -1,
    /* stick_x stick_y    */ -1, -1,
};
#endif

static char s_warning[64] = "";
extern "C" const char *board_pins_warning(void) { return s_warning; }

struct PinKey { const char *key; int8_t *field; const char *help; };
static const PinKey KEYS[] = {
    { "up",        &g_pins.up,        "joystick up" },
    { "down",      &g_pins.down,      "joystick down" },
    { "left",      &g_pins.left,      "joystick left" },
    { "right",     &g_pins.right,     "joystick right" },
    { "fire",      &g_pins.fire,      "fire button" },
    { "fire2",     &g_pins.fire2,     "extra fire button" },
    { "fire3",     &g_pins.fire3,     "extra fire button" },
    { "fire4",     &g_pins.fire4,     "extra fire button" },
    { "fire_b",    &g_pins.fireb,     "second fire button (bomb), optional; if -1, fire does both" },
    { "fire_b2",   &g_pins.fireb2,    "extra second fire button, optional" },
    { "fire_b3",   &g_pins.fireb3,    "extra second fire button, optional" },
    { "start",     &g_pins.start,     "START (hold START+COIN in a game = game list)" },
    { "coin",      &g_pins.coin,      "COIN / insert credit" },
    { "menu",      &g_pins.menu,      "MENU: tap = back, hold in a game = game list, hold at power-on = ROM drive" },
    { "nav_up",    &g_pins.nav_up,    "extra menu UP button (game list only), optional" },
    { "nav_down",  &g_pins.nav_down,  "extra menu DOWN button (game list only), optional" },
    { "joy_x",     &g_pins.joy_x,     "analog stick X (ADC GPIO 26-28), optional" },
    { "joy_y",     &g_pins.joy_y,     "analog stick Y (ADC GPIO 26-28), optional" },
    { "joy_click", &g_pins.joy_click, "analog stick click = fire, optional" },
    { "audio",     &g_pins.audio,     "PWM audio output (to an amplifier)" },
    { "amp",       &g_pins.amp,       "amplifier enable (driven HIGH while playing), optional" },
    { "backlight", &g_pins.backlight, "display backlight (driven HIGH), optional" },
};
static const int NKEYS = sizeof(KEYS) / sizeof(KEYS[0]);

/* The display is on these (TFT_eSPI build flags) — never let a mapping take them. */
static bool reserved_pin(int p) {
#ifdef TFT_SCLK
    if (p == TFT_SCLK) return true;
#endif
#ifdef TFT_MOSI
    if (p == TFT_MOSI) return true;
#endif
#ifdef TFT_CS
    if (p == TFT_CS) return true;
#endif
#ifdef TFT_DC
    if (p == TFT_DC) return true;
#endif
#ifdef TFT_RST
    if (p == TFT_RST) return true;
#endif
#ifdef PICO_STANDALONE
    /* Pico 2 board internals: SMPS mode, VBUS sense, LED, VSYS/3 ADC. */
    if (p == 23 || p == 24 || p == 25 || p == 29) return true;
#endif
    return false;
}

#ifdef PICO_STANDALONE
/* Parse "key = value" lines; ';' or '#' start a comment. Unknown keys and
 * invalid pins are reported once and keep their default. */
static void parse_ini(const char *text, size_t len) {
    const char *p = text, *end = text + len;
    int bad = 0;
    char firstBad[16] = "";
    while (p < end) {
        const char *eol = (const char *)memchr(p, '\n', end - p);
        if (!eol) eol = end;
        char line[96];
        size_t n = eol - p;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p = eol + 1;
        char *c = strpbrk(line, ";#");
        if (c) *c = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        for (char *t = k + strlen(k); t > k && (t[-1] == ' ' || t[-1] == '\t'); ) *--t = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (!*k) continue;
        char *vend;
        long val = strtol(v, &vend, 10);
        /* Pico 2 (RP2350A): GPIO 0-29 exist. */
        bool ok = (vend != v) && val >= -1 && val <= 29 && !(val >= 0 && reserved_pin((int)val));
        int i;
        for (i = 0; i < NKEYS; i++) if (strcasecmp(k, KEYS[i].key) == 0) break;
        if (i == NKEYS || !ok) {
            if (!bad) strncpy(firstBad, k, sizeof(firstBad) - 1);
            bad++;
            continue;
        }
        *KEYS[i].field = (int8_t)val;
    }
    if (bad) snprintf(s_warning, sizeof(s_warning), "ARCADE.INI: %d bad line(s), e.g. '%s'", bad, firstBad);
}
#endif

extern "C" void board_pins_load(void) {
#ifdef PICO_STANDALONE
    uint8_t *ini = nullptr;
    size_t n = 0;
    if (romfs_slurp("/ARCADE.INI", &ini, &n)) {
        parse_ini((const char *)ini, n);
        free(ini);
        Serial.println("[board] pins from ARCADE.INI");
    } else {
        Serial.println("[board] no ARCADE.INI - default pins");
    }
#endif
    Serial.printf("[board] up%d down%d left%d right%d fire%d start%d coin%d menu%d audio%d\n",
                  g_pins.up, g_pins.down, g_pins.left, g_pins.right, g_pins.fire,
                  g_pins.start, g_pins.coin, g_pins.menu, g_pins.audio);
    if (g_pins.backlight >= 0) {
        pinMode(g_pins.backlight, OUTPUT);
        digitalWrite(g_pins.backlight, HIGH);
    }
}

extern "C" const char *board_default_ini(void) {
    static char buf[2048];
    int n = snprintf(buf, sizeof(buf),
        "; Pico Galagino arcade - pin configuration\r\n"
        "; GPIO numbers; -1 = not connected. Buttons/directions are switches to GND\r\n"
        "; (internal pull-ups). Display (not set here): SPI0 SCLK 6, MOSI 7, CS 5,\r\n"
        "; DC 8, RST 9. Not usable: 23-25, 29 (Pico 2 internals). Edit, save,\r\n"
        "; eject the drive, then press MENU or START.\r\n\r\n");
    /* Current values: the defaults on a fresh board, or the user's mapping if
     * the drive is re-formatted after an ARCADE.INI was loaded. */
    for (int i = 0; i < NKEYS && n < (int)sizeof(buf); i++) {
        int v = *KEYS[i].field;
        n += snprintf(buf + n, sizeof(buf) - n, "%-10s = %-3d ; %s\r\n", KEYS[i].key, v, KEYS[i].help);
    }
    return buf;
}

#else   /* PicoCartReader: fixed wiring, no table */
extern "C" void board_pins_load(void) {}
extern "C" const char *board_default_ini(void) { return ""; }
extern "C" const char *board_pins_warning(void) { return ""; }
#endif
