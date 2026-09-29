/* pico_input.cpp — PicoGalagino input glue.
 *
 * Replaces upstream's raw digitalRead(BTN_*_PIN) reads (which target ESP32
 * pins that don't exist on our board) with reads against:
 *   - PCA9539 I2C expander (Player 1 stick + on-board A/B buttons)
 *   - Direct RP2350 GPIOs (SELECT/START/MENU)
 *
 * Activated by -DPICO_CUSTOM_INPUT (see config.h). When defined, the
 * built-in buttons_get() in galagino.cpp is compiled out and this
 * definition wins at link time.
 */

#include <Arduino.h>
#include "expander.h"
#include "emulation.h"    /* BUTTON_LEFT/RIGHT/UP/DOWN/FIRE/START/COIN/EXTRA */
#include "pico_audio.h"
#include "pico_handoff.h"

/* Direct-GPIO buttons on the PicoCartReader board (active LOW, pull-up). */
static const int PIN_START  = 0;    /* GP0 -> START */
static const int PIN_SELECT = 1;    /* GP1 -> COIN */
static const int PIN_MENU   = 37;   /* MENU+UP/DOWN = volume, tap = back */

/* Set by buttons_get() when MENU is tapped (released without an
 * UP/DOWN in between). Consumed by pico_input_menu_tapped(). */
static volatile bool g_menu_tapped = false;

/* Volume step for MENU+UP/DOWN — matches PicoCartReader. */
static const int VOLUME_CHORD_STEP = 10;

/* Hold START+SELECT this long during a game to return to the list. */
static const unsigned long EXIT_HOLD_MS = 500;

/* Set by pico_menu once a game is armed (defined in pico_core1.cpp). */
extern volatile int g_menu_done;

extern "C" bool pico_input_menu_tapped(void) {
    bool t = g_menu_tapped;
    g_menu_tapped = false;
    return t;
}

/* JoyCon analog stick — ADC channels on GP44 (X) and GP45 (Y). Center
 * calibrated at boot (assume stick is at rest). Anything > threshold
 * away from center in either axis triggers a digital direction. */
static const int PIN_JOY_X  = 44;
static const int PIN_JOY_Y  = 45;
static const int JOY_THRESHOLD = 900;   /* ~22% of 12-bit range */
static int joy_center_x = 2048;
static int joy_center_y = 2048;

#ifdef PICO_GALAGINO_LANDSCAPE
/* Runtime landscape flag from pico_config.cpp. C linkage matches its
 * definition. Declared at file scope so buttons_get can reference it
 * (C++ won't accept a linkage-spec inside a function body). */
extern "C" volatile int g_landscape_mode;
#endif

extern "C" void pico_input_init(void) {
    Serial.println("[input] pico_input_init");
    pinMode(PIN_SELECT, INPUT_PULLUP);
    pinMode(PIN_START,  INPUT_PULLUP);
    pinMode(PIN_MENU,   INPUT_PULLUP);
    bool ok = expander_init();
    Serial.printf("[input] PCA9539: %s\n", ok ? "OK" : "not found");

    /* JoyCon auto-center: sample the stick a few times at boot (it's at
     * rest during power-up) and use the average as the neutral position. */
    analogReadResolution(12);
    long sx = 0, sy = 0;
    const int N = 8;
    for (int i = 0; i < N; i++) {
        sx += analogRead(PIN_JOY_X);
        sy += analogRead(PIN_JOY_Y);
        delay(2);
    }
    joy_center_x = (int)(sx / N);
    joy_center_y = (int)(sy / N);
    Serial.printf("[input] JoyCon center: X=%d Y=%d\n", joy_center_x, joy_center_y);
}

/* Galagino calls this MANY times per frame — the emulated CPU polls
 * the button port whenever the game asks. Doing analogRead + I2C reads
 * on every call was costing 3-5 fps.
 *
 * Cache strategy: hold the cached state for up to 10 ms EXCEPT when the
 * PCA9539's INT pin has signalled a change. That way the I2C bus load
 * stays low when nothing is happening, but every button transition
 * (press or release) forces an immediate fresh read — no short-tap
 * dropouts inside a poll window, no software debounce. */
extern "C" unsigned char buttons_get(void) {
    static unsigned char cached_state = 0;
    static unsigned long last_ms = 0;
    unsigned long now = millis();
    /* 1 ms cache — human rapid-tap max ~10 Hz gives a press duration
     * around 30-50 ms. 1 kHz polling catches every edge with plenty of
     * margin. Any INT-line pulse from the PCA9539 bypasses the cache
     * immediately (state change → fresh read). */
    if (last_ms != 0 && (now - last_ms) < 1 && !expander_hasInterrupt())
        return cached_state;
    last_ms = now;

    uint16_t s = expander_readAll();   /* 16 bits, active LOW */
    unsigned char state = 0;

    /* Direct-GPIO controls */
    if (digitalRead(PIN_START)  == LOW) state |= BUTTON_START;
    if (digitalRead(PIN_SELECT) == LOW) state |= BUTTON_COIN;

    /* Player 1 joystick — PCA9539 P0.0-P0.4. */
    if (!(s & EXP_P1_UP))    state |= BUTTON_UP;
    if (!(s & EXP_P1_DOWN))  state |= BUTTON_DOWN;
    if (!(s & EXP_P1_LEFT))  state |= BUTTON_LEFT;
    if (!(s & EXP_P1_RIGHT)) state |= BUTTON_RIGHT;
    if (!(s & EXP_P1_FIRE))  state |= BUTTON_FIRE;

    /* On-board thumb buttons — both have external pull-ups on the PCB,
       so they're safe to poll. BTN_A duplicates FIRE (thumb-fire is
       handy for arcade play), BTN_B duplicates COIN. */
    if (!(s & EXP_BTN_A))    state |= BUTTON_FIRE;
    if (!(s & EXP_BTN_B))    state |= BUTTON_COIN;


    /* JoyCon push-button (P1.5) NOT mapped — the pin carries an
       active signal from the JoyCon FPC that flickers HIGH↔LOW on
       every I2C read, causing phantom fires. Would need proper JoyCon
       protocol code (see GameAdapterRGBPico) to use those buttons. */

    /* JoyCon analog stick → digital directions.
     *
     * The PCA9539 joystick above is NOT rotated — an external plugged-in
     * stick's orientation is up to how the user connected it. The JoyCon
     * is soldered/mounted to the PCB, aligned for LANDSCAPE orientation.
     * In PORTRAIT mode the same physical push hits the game 90° off, so
     * rotate its axes 90° in portrait only. */
    int jx = analogRead(PIN_JOY_X) - joy_center_x;
    int jy = analogRead(PIN_JOY_Y) - joy_center_y;
#ifdef PICO_GALAGINO_LANDSCAPE
    if (!g_landscape_mode) {
        int rjx =  jy;
        int rjy = -jx;
        jx = rjx;
        jy = rjy;
    }
#endif
    if (jx < -JOY_THRESHOLD) state |= BUTTON_LEFT;
    if (jx >  JOY_THRESHOLD) state |= BUTTON_RIGHT;
    if (jy < -JOY_THRESHOLD) state |= BUTTON_UP;
    if (jy >  JOY_THRESHOLD) state |= BUTTON_DOWN;

    /* MENU chord — same behaviour as PicoCartReader's emu_GetPad():
     * MENU + UP/DOWN steps the volume (directions hidden from the game
     * while MENU is held); a plain MENU tap is reported on release. */
    {
        static bool menu_down = false, chorded = false;
        static int  last_dir  = 0;
        if (digitalRead(PIN_MENU) == LOW) {
            if (!menu_down) { menu_down = true; chorded = false; last_dir = 0; }
            int dir = (state & BUTTON_UP) ? +1 : (state & BUTTON_DOWN) ? -1 : 0;
            if (dir && dir != last_dir) {
                int v = pico_audio_get_volume_pct() + dir * VOLUME_CHORD_STEP;
                if (v < 0)   v = 0;
                if (v > 100) v = 100;
                pico_audio_set_volume_pct(v);
                handoff_volume_write(v);
                chorded = true;
            }
            last_dir = dir;
            state &= ~(BUTTON_UP | BUTTON_DOWN | BUTTON_LEFT | BUTTON_RIGHT);
        } else if (menu_down) {
            menu_down = false;
            if (!chorded) g_menu_tapped = true;
        }
    }

    /* Hold START+SELECT during a game → back to the game list. */
    {
        static unsigned long held_since = 0;
        if (g_menu_done && (state & BUTTON_START) && (state & BUTTON_COIN)) {
            if (held_since == 0) held_since = now | 1;
            else if (now - held_since >= EXIT_HOLD_MS) handoff_relaunch_arcade();
        } else {
            held_since = 0;
        }
    }

    cached_state = state;
    return state;
}
