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

/* Direct-GPIO buttons on the PicoCartReader board (active LOW, pull-up). */
static const int PIN_SELECT = 0;    /* -> COIN */
static const int PIN_START  = 1;    /* -> START */
static const int PIN_MENU   = 37;   /* unused by Galagino right now */

/* JoyCon analog stick — ADC channels on GP44 (X) and GP45 (Y). Center
 * calibrated at boot (assume stick is at rest). Anything > threshold
 * away from center in either axis triggers a digital direction. */
static const int PIN_JOY_X  = 44;
static const int PIN_JOY_Y  = 45;
static const int JOY_THRESHOLD = 900;   /* ~22% of 12-bit range */
static int joy_center_x = 2048;
static int joy_center_y = 2048;

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
 * on every call was costing 3-5 fps. We now cache the packed state
 * and only refresh at ~2 ms granularity (still plenty responsive for
 * a 60 Hz game). */
extern "C" unsigned char buttons_get(void) {
    static unsigned char cached_state = 0;
    static unsigned long last_ms = 0;
    unsigned long now = millis();
    /* 10 ms cache: 100 polls/sec is well above any human input rate,
       and it caps I2C bus load at ~1.5% (vs ~7% at 2 ms). */
    if (now - last_ms < 10 && last_ms != 0) return cached_state;
    last_ms = now;

    uint16_t s = expander_readAll();   /* 16 bits, active LOW */
    unsigned char state = 0;

    /* Direct-GPIO controls */
    if (digitalRead(PIN_START)  == LOW) state |= BUTTON_START;
    if (digitalRead(PIN_SELECT) == LOW) state |= BUTTON_COIN;

    /* MENU button (GP37) → soft-reboot back to our boot menu. Requires
       ~250 ms sustained press so a brief bump doesn't drop the user's
       game. Reboot lands back in setup() → our picker. */
    static unsigned long menu_press_start = 0;
    if (digitalRead(PIN_MENU) == LOW) {
        if (menu_press_start == 0) menu_press_start = millis();
        else if (millis() - menu_press_start > 250) {
            Serial.println("[input] MENU held — rebooting to picker");
            delay(50);
            rp2040.reboot();
        }
    } else {
        menu_press_start = 0;
    }

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

    /* JoyCon analog stick → digital directions. */
    int jx = analogRead(PIN_JOY_X) - joy_center_x;
    int jy = analogRead(PIN_JOY_Y) - joy_center_y;
    if (jx < -JOY_THRESHOLD) state |= BUTTON_LEFT;
    if (jx >  JOY_THRESHOLD) state |= BUTTON_RIGHT;
    if (jy < -JOY_THRESHOLD) state |= BUTTON_UP;
    if (jy >  JOY_THRESHOLD) state |= BUTTON_DOWN;

    cached_state = state;
    return state;
}
