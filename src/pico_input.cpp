/* pico_input.cpp — Pico2Arcade input glue.
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
#include "config.h"       /* ENABLE_<game>: the MCH_ ids in emulation.h (4-way games) */
#include "emulation.h"    /* BUTTON_LEFT/RIGHT/UP/DOWN/FIRE/START/COIN/EXTRA */
#include "pico_audio.h"
#include "pico_handoff.h"
#include "pico_board.h"
#include "pico_hiscore.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"

#ifdef PICO_BOARD_TABLE
/* Quad Ultimate CX / standalone Pico 2 (all active LOW, pull-up): no PCA9539 —
 * every control is a direct GPIO from the runtime pin table (pico_board.h;
 * Quad profile: port 1 DB9 28-31 fire 12, Red/Blue/Joy-Con click = fire,
 * White = START, Black = COIN, gray Menu). -1 = not connected. */
#define PIN_START  g_pins.start
#define PIN_SELECT g_pins.coin
#define PIN_MENU   g_pins.menu
static inline bool pin_low(int p) { return p >= 0 && digitalRead(p) == LOW; }
#else
/* Direct-GPIO buttons on the PicoCartReader board (active LOW, pull-up). */
static const int PIN_START  = 0;    /* GP0 -> START */
static const int PIN_SELECT = 1;    /* GP1 -> COIN */
static const int PIN_MENU   = 37;   /* MENU+UP/DOWN = volume, tap = back */
#endif

/* Set by buttons_get() when MENU is tapped (pressed and released).
 * Consumed by pico_input_menu_tapped(). */
static volatile bool g_menu_tapped = false;

/* (The MENU+UP/DOWN volume chord is gone — volume is a row in the game
 * list, pico_menu.cpp; there is no in-game volume control.) */

/* Hold START+SELECT this long during a game to return to the list. */
static const unsigned long EXIT_HOLD_MS = 500;
/* Hold MENU this long during a game → game list. */
static const unsigned long MENU_EXIT_HOLD_MS = 1000;

/* Two-button games (laser + bomb): set by pico_menu for the chosen game.
 * Then the B buttons report BUTTON_FIRE2 instead of their usual job, and a
 * single-button external joystick fires both. */
extern "C" { volatile int g_two_fire = 0; }
#ifdef PICO_HDMI
extern "C" volatile int g_scale_mode, g_stretch;   /* video.cpp: Scale / Width (serial keys z / v) */
extern "C" volatile int g_show_fps;                /* pico_config.cpp: Show FPS (serial key f) */
extern "C" void video_hdmi_mode_key(void);         /* video.cpp: serial keys m / k (screen mode) */
extern "C" void video_hdmi_keep_key(void);
extern "C" void video_hdmi_audio_poll(void);       /* video.cpp: the HDMI sound feed (game list) */
#endif

#if defined(PICO_TINY_SCREEN) || defined(PICO_GALAGINO_LANDSCAPE_AREA)
#  define INGAME_WIDTH_TAP 1
extern "C" volatile int g_stretch;   /* video.cpp: landscape width index */
#endif
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
#ifdef PICO_BOARD_TABLE
#define PIN_JOY_X  g_pins.joy_x
#define PIN_JOY_Y  g_pins.joy_y
#else
static const int PIN_JOY_X  = 44;
static const int PIN_JOY_Y  = 45;
#endif
#ifdef PICO_HOSTED
/* Quad / Duo 5200 Joy-Con (Hall stick), as GameAdapterRGBPico joycon5200.cpp:
 * it rests at about X 2257 / Y 2081, not the 2048 midpoint, and right after
 * power-on reads low (~1335) until it settles. A centre captured then sat a
 * few hundred counts low, so a stick at rest was already part way to RIGHT
 * and a light touch crossed 512. Now the boot sample is used only if it is
 * in 1700-2500 and steady, else the measured rest; a direction needs 700
 * counts and lets go below 420 (no flicker at the edge). */
static const int JOY_THRESHOLD = 700;
static const int JOY_RELEASE   = JOY_THRESHOLD * 6 / 10;
static int joy_center_x = 2257;
static int joy_center_y = 2081;
static const int JOY_CENTER_MIN = 1700, JOY_CENTER_MAX = 2500, JOY_NOISE_MAX = 200;
#else
/* A direction triggers at 25% of the stick's travel from centre (2048 =
 * full deflection), so diagonals come easily. */
static const int JOY_THRESHOLD = 512;
static const int JOY_RELEASE   = JOY_THRESHOLD;
static int joy_center_x = 2048;
static int joy_center_y = 2048;
#endif

#ifdef PICO_BOARD_TABLE
/* ── Atari 5200 controller analog stick (g_pins.stick_x / stick_y) ──────
 * The 5200 pots are read the way the Duo 5200 firmware does it (GameAdapter
 * pins.cpp): each pin's capacitor rests discharged (OUTPUT LOW); released to
 * INPUT it charges through the pot, and the time until the pin reads HIGH
 * grows with the resistance — longer = right (X) / down (Y). Measured on the
 * Duo 5200 (GameAdapterRGBPico / Pico2Atari8 bench, 2026-11-18): about
 * 0.3 ms full left / up, ~1.8 ms at rest, up to ~7.1 ms pushed hard right /
 * down (the host's PIO counts 0.8 us each: ~400 / ~2300 / ~8850). The old
 * read assumed 0-1.3 ms and timed out at 2 ms, so right / down mostly timed
 * out and didn't register.
 * Non-blocking (no PIO: it clashed with the arcade's other users): the caps
 * are released together and a rising-edge GPIO interrupt on core 0 stamps
 * each axis's charge time; the next poll collects the result. Nothing waits
 * on the charge, so a 7 ms read costs core 0 (which renders every frame) no
 * time. One read is started at most every 16 ms, only from core 0.
 * Directions are digital: the range is learned from the min/max seen (the
 * nominal range until the stick has moved); a direction triggers at 25 %
 * of the travel, so diagonals come easily.
 * Centre = where the stick rests at its first reading (like the Joy-Con's
 * boot calibration), and each side's 25 % is of the travel seen on THAT side
 * of it. (Assuming the nominal centre made a stick that rests at a longer
 * charge time read as DOWN at rest — the game list kept scrolling down until
 * UP widened the learned range.) Unplugged (both axes time out) = start over. */
static const uint32_t RC_TIMEOUT_US = 8000;     /* no controller / past any stick (max seen ~7.1 ms) */
static const uint32_t RC_NOMINAL_US = 1334;     /* nominal travel for the first thresholds (see below) */
static const uint32_t RC_PERIOD_US  = 15000;    /* one read per frame at most (a game polls once a frame) */
static const uint32_t RC_DRAIN_US   = 200;      /* caps parked LOW at least this long (1.5 k x 22 nF: ~33 us tau) */
static volatile unsigned char s_stickDirs = 0;  /* BUTTON_UP/DOWN/LEFT/RIGHT from the stick */

/* Read state: written by the edge interrupt (core 0) and rc_stick_read (core 0). */
static volatile uint32_t s_rcT0 = 0;            /* when the caps were released */
static volatile uint32_t s_rcT[2] = { 0, 0 };   /* charge time per axis (us + 1), 0 = not yet */
static bool     s_rcRunning = false;
static bool     s_rcIrqReady = false;
static uint32_t s_rcLastStart = 0, s_rcParkedAt = 0;

static void rc_stick_init(void) {
    const int pins[2] = { g_pins.stick_x, g_pins.stick_y };
    for (int p : pins) {
        if (p < 0) continue;
        gpio_init(p);
        gpio_disable_pulls(p);
        gpio_put(p, 0);
        gpio_set_dir(p, GPIO_OUT);              /* park discharged */
    }
    s_rcParkedAt = time_us_32();
}

/* Rising edge on a stick pin: that axis's cap has charged. */
static void rc_stick_irq(void) {
    const int pins[2] = { g_pins.stick_x, g_pins.stick_y };
    uint32_t now = time_us_32();
    for (int a = 0; a < 2; a++) {
        if (!(gpio_get_irq_event_mask(pins[a]) & GPIO_IRQ_EDGE_RISE)) continue;
        gpio_acknowledge_irq(pins[a], GPIO_IRQ_EDGE_RISE);
        gpio_set_irq_enabled(pins[a], GPIO_IRQ_EDGE_RISE, false);
        if (!s_rcT[a]) s_rcT[a] = now - s_rcT0 + 1;
    }
}

/* Park both caps (OUTPUT LOW) and stop the edge interrupts. */
static void rc_stick_park(void) {
    const int pins[2] = { g_pins.stick_x, g_pins.stick_y };
    for (int a = 0; a < 2; a++) {
        gpio_set_irq_enabled(pins[a], GPIO_IRQ_EDGE_RISE, false);
        gpio_put(pins[a], 0);
        gpio_set_dir(pins[a], GPIO_OUT);
    }
    s_rcParkedAt = time_us_32();
}

static void rc_stick_decide(const uint32_t t[2]);
static bool four_way_game(void);               /* below, with buttons_get */
static const int32_t STICK_DIR_PCT    = 25;   /* % of a side's travel for a direction */
static const int32_t STICK_REL_PCT    = 15;   /* ... and it lets go below this (no flicker at the edge) */
static const int32_t STICK_SWITCH_PCT = 20;   /* 4-way: the other axis must lead by this to take over */
/* 4-way rule for the 5200 stick (both on the bench, 2026-11-18):
 *   0 = strongest axis: the axis pushed further; the other takes over only
 *       once it leads by STICK_SWITCH_PCT;
 *   1 = combined: as 0, and also an axis that newly comes on takes over
 *       (moving right, push into up-right at a junction -> UP). */
#ifndef STICK_4WAY_MODE
#define STICK_4WAY_MODE 1
#endif

static void rc_stick_read(void) {
    if (g_pins.stick_x < 0 || g_pins.stick_y < 0) return;
    const int pins[2] = { g_pins.stick_x, g_pins.stick_y };
    if (!s_rcIrqReady) {                        /* first call: core 0, so the interrupt is core 0's */
        gpio_add_raw_irq_handler_masked64((1ull << pins[0]) | (1ull << pins[1]), rc_stick_irq);
        irq_set_enabled(IO_IRQ_BANK0, true);
        s_rcIrqReady = true;
    }
    uint32_t now = time_us_32();

    if (s_rcRunning) {
        bool done = s_rcT[0] && s_rcT[1];
        if (!done && now - s_rcT0 < RC_TIMEOUT_US) return;   /* still charging */
        rc_stick_park();
        s_rcRunning = false;
        uint32_t t[2] = { s_rcT[0], s_rcT[1] };
        rc_stick_decide(t);
        /* Start the next read now rather than on the next poll — a game polls
         * once a frame, so waiting would read only every other frame. The
         * short drain is all this read ever blocks for. */
        delayMicroseconds(RC_DRAIN_US);
        now = time_us_32();
    }

    if (s_rcLastStart != 0 && now - s_rcLastStart < RC_PERIOD_US) return;
    if (now - s_rcParkedAt < RC_DRAIN_US) return;               /* let the caps drain */

    /* Release both caps and stamp the start together: no interrupt in between. */
    s_rcT[0] = s_rcT[1] = 0;
    for (int a = 0; a < 2; a++) {
        gpio_acknowledge_irq(pins[a], GPIO_IRQ_EDGE_RISE);
        gpio_set_irq_enabled(pins[a], GPIO_IRQ_EDGE_RISE, true);
    }
    uint32_t irqs = save_and_disable_interrupts();
    s_rcT0 = time_us_32();
    gpio_set_dir(pins[0], GPIO_IN);
    gpio_set_dir(pins[1], GPIO_IN);
    restore_interrupts(irqs);
    s_rcLastStart = s_rcT0;
    s_rcRunning = true;
}

/* A finished read (t = charge time + 1 per axis, 0 = timed out) to directions. */
static void rc_stick_decide(const uint32_t tIn[2]) {
    static uint32_t lo[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu }, hi[2] = { 0, 0 };
    static int32_t  rest[2] = { -1, -1 };       /* charge time with the stick at rest */
    uint32_t t[2] = { tIn[0], tIn[1] };
    if (!t[0] && !t[1]) {                       /* no controller: forget its calibration */
        for (int a = 0; a < 2; a++) { lo[a] = 0xFFFFFFFFu; hi[a] = 0; rest[a] = -1; }
        s_stickDirs = 0;
        return;
    }

    /* Each axis as a percentage of the travel seen on its side of rest (until
     * a side has moved at least a quarter of the nominal travel, of the
     * nominal half-travel, ~667 us). A direction needs STICK_DIR_PCT (25 %:
     * clear of noise, easy to reach). */
    int32_t pct[2] = { 0, 0 };                  /* signed: - = left / up */
    for (int a = 0; a < 2; a++) {
        if (!t[a]) continue;                    /* timed out: no controller on this axis */
        if (rest[a] < 0) rest[a] = (int32_t)t[a];   /* first reading: the stick at rest */
        if (t[a] < lo[a]) lo[a] = t[a];
        if (t[a] > hi[a]) hi[a] = t[a];
        const int32_t minSpan = RC_NOMINAL_US / 4, nomSpan = RC_NOMINAL_US / 2;
        int32_t spanNeg = rest[a] - (int32_t)lo[a], spanPos = (int32_t)hi[a] - rest[a];
        if (spanNeg < minSpan) spanNeg = nomSpan;
        if (spanPos < minSpan) spanPos = nomSpan;
        int32_t d = (int32_t)t[a] - rest[a];
        pct[a] = d > 0 ? d * 100 / spanPos : d * 100 / spanNeg;
    }

    /* 4-way games: only the axis pushed further goes through, so a stick held
     * near the diagonal sends one direction (two at once and Pac-Man doesn't
     * take the turn). It hands over to the other axis only once that one is
     * ahead by more than STICK_SWITCH_PCT, so the diagonal doesn't flicker
     * between them (user's design, 2026-11-18). 8-way games keep both. */
    static int axis = -1;                       /* 0 = X, 1 = Y, -1 = neither */
    static bool held[2] = { false, false };     /* per axis, with hysteresis */
    bool fresh[2];
    for (int a = 0; a < 2; a++) {
        bool was = held[a];
        held[a] = abs(pct[a]) > (was ? STICK_REL_PCT : STICK_DIR_PCT);
        fresh[a] = held[a] && !was;
    }
    bool on[2] = { held[0], held[1] };
    if (g_menu_done && four_way_game()) {
        if (on[0] && on[1]) {
            if (axis < 0) axis = abs(pct[0]) >= abs(pct[1]) ? 0 : 1;
#if STICK_4WAY_MODE == 1
            else if (fresh[1 - axis]) axis = 1 - axis;
#endif
            else if (abs(pct[1 - axis]) > abs(pct[axis]) + STICK_SWITCH_PCT) axis = 1 - axis;
            on[1 - axis] = false;
        } else {
            axis = on[0] ? 0 : on[1] ? 1 : -1;
        }
    }

    unsigned char dirs = 0;
    if (on[0]) dirs |= pct[0] > 0 ? BUTTON_RIGHT : BUTTON_LEFT;
    if (on[1]) dirs |= pct[1] > 0 ? BUTTON_DOWN  : BUTTON_UP;
    s_stickDirs = dirs;
}

#ifdef PICO_USB_HOST
/* ── USB gamepads / keyboard (Fruit Jam USB-A ports) + the serial monitor ──
 * The host side is the module shared with Pico2Atari8, unchanged
 * (extern/usb_pads: host start, HID descriptor parser, pad layout, unplug
 * recovery, power-cycle after flash writes; its C half is compiled in
 * pico_freertos_stubs.c). Compiled here, not as a file of its own: an extra
 * object file can reorder the other boards' linker veneers.
 * Everything USB runs on core 0 (the module's rule: all calls from the core
 * that started it; its 1 ms frame timer runs there at the highest priority,
 * away from core 1's HDMI line interrupt). Core 0 polls — the game list's
 * buttons_get, and in a game update_screen's frame waits (pico_input_poll) —
 * and leaves the result in s_usbState as BUTTON_* bits; buttons_get on
 * either core ORs it in.
 * Pad mapping (both pads, OR'd — Galagino reads one player): D-pad / hat and
 * the left stick = directions, B (HID button 1, the main one) and X = FIRE,
 * A and Y = FIRE2 in two-button games (else FIRE), START = START, SELECT =
 * COIN. START + SELECT held = the game list (the START + COIN rule below).
 * Keyboard (boot protocol, not tested on hardware): arrows, Space / Left Ctrl
 * fire, Left Alt fire 2, Enter / 1 START, 5 / C COIN, Esc = the game list.
 * Serial monitor (a terminal on the USB-C port): arrows or w a s d, Space
 * fire, b fire 2, Enter or 1 START, c or 5 COIN, x the game list; + - volume,
 * A H S B output (auto, headphones, speaker, both), I the sound report. A
 * terminal sends no key-up, so a key counts as held for 150 ms after it
 * last came (its auto-repeat keeps it held). */
#include "usb_pads.cpp"

static volatile unsigned char s_usbState = 0;   /* BUTTON_* from pads, keyboard, serial */
static bool s_usbStarted = false;
static const uint32_t SERIAL_HOLD_MS = 150;
static uint32_t s_serUntil[8];                  /* per BUTTON_* bit: held until (millis) */

static void serial_press(unsigned char bit) {
    for (int i = 0; i < 8; i++) if (bit & (1u << i)) s_serUntil[i] = millis() + SERIAL_HOLD_MS;
}

static void serial_poll(void) {
    static int esc = 0;                         /* 1 after ESC, 2 after ESC [ */
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (esc == 1) { esc = (c == '[') ? 2 : 0; continue; }
        if (esc == 2) {
            esc = 0;
            if (c == 'A') serial_press(BUTTON_UP);
            else if (c == 'B') serial_press(BUTTON_DOWN);
            else if (c == 'C') serial_press(BUTTON_RIGHT);
            else if (c == 'D') serial_press(BUTTON_LEFT);
            continue;
        }
        switch (c) {
        case 27: esc = 1; break;
        case 'w': serial_press(BUTTON_UP); break;
        case 's': serial_press(BUTTON_DOWN); break;
        case 'a': serial_press(BUTTON_LEFT); break;
        case 'd': serial_press(BUTTON_RIGHT); break;
        case ' ': serial_press(BUTTON_FIRE); break;
        case 'b': serial_press(g_two_fire ? BUTTON_FIRE2 : BUTTON_FIRE); break;
        case '\r': case '\n': case '1': serial_press(BUTTON_START); break;
        case 'c': case '5': serial_press(BUTTON_COIN); break;
        case 'x': if (g_menu_done) pico_hiscore_exit_to_list(); break;
#ifdef PICO_HDMI
        /* Scale (Smooth / Sharp / Drop) and Width (372 ... 640 crop) in a game:
         * the picture changes at the next frame and is saved (video.cpp) */
        case 'z': if (g_menu_done) g_scale_mode = (g_scale_mode + 1) % 3; break;
        case 'v': if (g_menu_done) g_stretch = (g_stretch + 1) % 8; break;
        case 'f': g_show_fps = !g_show_fps; break;   /* the FPS box on / off (not saved: the Show FPS row is) */
        /* the next HDMI screen mode (video.cpp): in the game list as FIRE on
         * the Screen row (10 s to keep it with FIRE / START); in a game at
         * once, k keeps it within 10 s, else it goes back */
        case 'm': video_hdmi_mode_key(); break;
        case 'k': video_hdmi_keep_key(); break;
#endif
        default: pico_audio_key(c); break;      /* + - A H S B I */
        }
    }
}

static unsigned char serial_state(void) {
    unsigned char st = 0;
    const uint32_t now = millis();
    for (int i = 0; i < 8; i++) if ((int32_t)(s_serUntil[i] - now) > 0) st |= (unsigned char)(1u << i);
    return st;
}

/* HID keyboard usages */
static const uint8_t KEY_ENTER = 0x28, KEY_ESC = 0x29, KEY_SPACE = 0x2C, KEY_1 = 0x1E, KEY_5 = 0x22,
                     KEY_C = 0x06, KEY_RIGHT = 0x4F, KEY_LEFT = 0x50, KEY_DOWN = 0x51, KEY_UP = 0x52;

/* (2026-11-11, later: the USB host moved to core 1, the emulator's, so the
 * HDMI line interrupt could move to core 0 — Pico2Atari8's split: the
 * emulator and USB on one core, HDMI on the other. The line interrupt had
 * taken 46 %, then 15 %, of the emulator's core, and the Scale modes cost
 * more. The rule stays: never the PIO-USB frame timer and the HDMI line
 * interrupt on one core.) Core 0 lets core 1 start the host once its own USB
 * device side is up (pico_input_init: s_usbGo); core 1 then polls it —
 * setup1's wait before a game, emulation_task once a frame in one — and
 * leaves the pads in s_usbState. The serial monitor stays on core 0. */
static volatile bool s_usbGo = false;
static volatile bool s_usbPowerCycle = false;   /* core 0 asks (a flash write), core 1 does it */

/* Core 1: start the host (once), then its task and the pads. */
extern "C" void pico_input_core1_usb_poll(void) {
    if (!s_usbStarted) {
        if (!s_usbGo) return;
        static const usb_pads_config_t cfg = USB_PADS_FRUITJAM;   /* D+ 1, 5 V on 11, PIO 2, DMA 11 */
        usb_pads_begin(&cfg);                   /* (its frame timer: on this core) */
        s_usbStarted = true;
    }
    if (s_usbPowerCycle) { s_usbPowerCycle = false; usb_pads_power_cycle(); }
    usb_pads_task();
    unsigned char st = 0;
    const unsigned char B = g_two_fire ? BUTTON_FIRE2 : BUTTON_FIRE;
    for (int n = 0; n < 2; n++) {
        if (!usb_pads_connected(n)) continue;
        const uint32_t b = usb_pads_buttons(n) | usb_pads_stick_dirs(n);
        if (b & USBP_UP)     st |= BUTTON_UP;
        if (b & USBP_DOWN)   st |= BUTTON_DOWN;
        if (b & USBP_LEFT)   st |= BUTTON_LEFT;
        if (b & USBP_RIGHT)  st |= BUTTON_RIGHT;
        if (b & (USBP_B | USBP_X)) st |= BUTTON_FIRE;
        if (b & (USBP_A | USBP_Y)) st |= B;
        if (b & USBP_START)  st |= BUTTON_START;
        if (b & USBP_SELECT) st |= BUTTON_COIN;
    }
    if (usb_pads_keyboard()) {
        if (usb_pads_kbd_has(KEY_UP))    st |= BUTTON_UP;
        if (usb_pads_kbd_has(KEY_DOWN))  st |= BUTTON_DOWN;
        if (usb_pads_kbd_has(KEY_LEFT))  st |= BUTTON_LEFT;
        if (usb_pads_kbd_has(KEY_RIGHT)) st |= BUTTON_RIGHT;
        const uint8_t mods = usb_pads_kbd_report()[0];
        if (usb_pads_kbd_has(KEY_SPACE) || (mods & 0x01)) st |= BUTTON_FIRE;   /* Left Ctrl */
        if (mods & 0x04) st |= B;                                              /* Left Alt */
        if (usb_pads_kbd_has(KEY_ENTER) || usb_pads_kbd_has(KEY_1)) st |= BUTTON_START;
        if (usb_pads_kbd_has(KEY_5) || usb_pads_kbd_has(KEY_C)) st |= BUTTON_COIN;
        if (usb_pads_kbd_has(KEY_ESC) && g_menu_done) pico_hiscore_exit_to_list();
    }
    s_usbState = st;
}

/* Core 0: the serial monitor (its keys are held in s_serUntil). */
static void usb_poll(void) {
    serial_poll();
}

/* After a flash write (core 0, interrupts were off): the USB devices
 * suspended meanwhile and some never wake — the ports are power-cycled
 * (by core 1, the host's core). */
extern "C" void pico_input_flash_written(void) { s_usbPowerCycle = true; }
#endif  /* PICO_USB_HOST */

/* Core 0 (loop()): keep the 5200 stick fresh while core 1 runs a game.
 * (USB host builds: also the pads and the serial monitor — called from
 * update_screen's frame waits as well.) */
extern "C" void pico_input_poll(void) {
    rc_stick_read();
#ifdef PICO_USB_HOST
    usb_poll();
#endif
}
#else
extern "C" void pico_input_poll(void) {}
#endif

#ifdef PICO_GALAGINO_LANDSCAPE
/* Runtime landscape flag from pico_config.cpp. C linkage matches its
 * definition. Declared at file scope so buttons_get can reference it
 * (C++ won't accept a linkage-spec inside a function body). */
extern "C" volatile int g_landscape_mode;
#endif

extern "C" void pico_input_init(void) {
    Serial.println("[input] pico_input_init");
#ifdef PICO_BOARD_TABLE
    /* No expander: on the Quad GP2/3 (its I2C) are side buttons and GP36 (its
     * INT) is a DB9 line, so it must not be initialised here. */
    const int tablePins[] = { g_pins.start, g_pins.coin, g_pins.menu, g_pins.nav_up, g_pins.nav_down,
                              g_pins.up, g_pins.down, g_pins.left, g_pins.right,
                              g_pins.fire, g_pins.fire2, g_pins.fire3, g_pins.fire4,
                              g_pins.fireb, g_pins.fireb2, g_pins.fireb3,
                              g_pins.joy_click };
    for (int p : tablePins) if (p >= 0) pinMode(p, INPUT_PULLUP);
#ifdef PICO_DUO
    const int db9Pins[] = { DUO_DB9_UP, DUO_DB9_DOWN, DUO_DB9_LEFT, DUO_DB9_RIGHT };
    for (int p : db9Pins) pinMode(p, INPUT_PULLUP);
#endif
    rc_stick_init();
#ifdef PICO_USB_HOST
    s_usbGo = true;      /* core 1 starts the USB host now (pico_input_core1_usb_poll) */
#endif
#else
    pinMode(PIN_SELECT, INPUT_PULLUP);
    pinMode(PIN_START,  INPUT_PULLUP);
    pinMode(PIN_MENU,   INPUT_PULLUP);
    bool ok = expander_init();
    Serial.printf("[input] PCA9539: %s\n", ok ? "OK" : "not found");
#endif

    /* JoyCon auto-center: sample the stick a few times at boot (it's at
     * rest during power-up) and use the average as the neutral position. */
#ifdef PICO_BOARD_TABLE
    if (PIN_JOY_X < 0 || PIN_JOY_Y < 0) return;     /* no analog stick */
#endif
    analogReadResolution(12);
    long sx = 0, sy = 0;
    const int N = 8;
    int lox = 4095, hix = 0, loy = 4095, hiy = 0;
    for (int i = 0; i < N; i++) {
        int x = analogRead(PIN_JOY_X), y = analogRead(PIN_JOY_Y);
        sx += x; sy += y;
        if (x < lox) lox = x;
        if (x > hix) hix = x;
        if (y < loy) loy = y;
        if (y > hiy) hiy = y;
        delay(2);
    }
#ifdef PICO_HOSTED
    {
        int cx = (int)(sx / N), cy = (int)(sy / N);
        bool ok = cx >= JOY_CENTER_MIN && cx <= JOY_CENTER_MAX && cy >= JOY_CENTER_MIN && cy <= JOY_CENTER_MAX
               && hix - lox <= JOY_NOISE_MAX && hiy - loy <= JOY_NOISE_MAX;
        if (ok) { joy_center_x = cx; joy_center_y = cy; }      /* else keep the measured rest */
        Serial.printf("[input] JoyCon rest X=%d Y=%d (%s)\n", cx, cy, ok ? "used" : "unsettled, default kept");
    }
#else
    (void)lox; (void)hix; (void)loy; (void)hiy;
    joy_center_x = (int)(sx / N);
    joy_center_y = (int)(sy / N);
#endif
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
/* 4-way games (Pac-Man, Ms. Pac-Man, Donkey Kong / Jr., Frogger, Dig Dug) were
 * built for a restricted stick that can't send two directions at once; given
 * up + right together Pac-Man doesn't take the turn. An analog stick held on
 * the diagonal sends both (the Duo 5200 controller does at full travel,
 * 2026-11-18: "holding up-right where there's a path up, it won't go up").
 * As MAME's 4-way joystick: of two directions held, only the most recently
 * pressed axis goes through — sweep from right to up-right and the game sees
 * UP (Pac-Man keeps running right until the turn opens). Other games keep
 * their diagonals (Xevious, 1942, Time Pilot, Scramble...). */
static bool four_way_game(void) {
#ifndef SINGLE_MACHINE
    switch (machine) {
#ifdef ENABLE_PACMAN
        case MCH_PACMAN:
#endif
#ifdef ENABLE_MSPACMAN
        case MCH_MSPACMAN:
#endif
#ifdef ENABLE_DKONG
        case MCH_DKONG:
#endif
#ifdef ENABLE_DKONGJR
        case MCH_DKONGJR:
#endif
#ifdef ENABLE_FROGGER
        case MCH_FROGGER:
#endif
#ifdef ENABLE_DIGDUG
        case MCH_DIGDUG:
#endif
            return true;
        default:
            return false;
    }
#else
    return false;
#endif
}

static unsigned char four_way(unsigned char state) {
    const unsigned char V = BUTTON_UP | BUTTON_DOWN, H = BUTTON_LEFT | BUTTON_RIGHT;
    static unsigned char prev = 0;     /* directions held last call (before filtering) */
    static unsigned char keep = 0;     /* V or H: the axis that goes through on a diagonal */
    const unsigned char dirs = state & (V | H), fresh = dirs & ~prev;
    prev = dirs;
    if ((dirs & V) && (dirs & H)) {
        if (fresh & V)      keep = V;  /* the axis pressed last wins */
        else if (fresh & H) keep = H;
        else if (!keep)     keep = V;
        state &= (unsigned char)~(keep == V ? H : V);
    } else {
        keep = (dirs & V) ? V : (dirs & H) ? H : 0;
    }
    return state;
}

extern "C" unsigned char PICO_HOT_RAM(buttons_get)(void) {
    static unsigned char cached_state = 0;
    static unsigned long last_ms = 0;
    unsigned long now = millis();
#ifdef A8_MENU_MUSIC
    /* Fruit Jam, the game list (core 0): its sound is fed from here - every
     * list screen polls the buttons (pico_audio.cpp menu sound; the HDMI
     * sound's packets too, as the frame wait does in a game) */
    /* (Since: over HDMI a 4 ms timer feeds the list's sound and the packets
     * - pico_audio.cpp menu_tick; here only the I2S feed and the jack.) */
    if (!g_menu_done && get_core_num() == 0) {
        pico_audio_menu_poll();
    }
#endif
    /* 1 ms cache — human rapid-tap max ~10 Hz gives a press duration
     * around 30-50 ms. 1 kHz polling catches every edge with plenty of
     * margin. Any INT-line pulse from the PCA9539 bypasses the cache
     * immediately (state change → fresh read). */
#ifdef PICO_BOARD_TABLE
    if (last_ms != 0 && (now - last_ms) < 1)     /* all direct GPIO — cheap */
        return cached_state;
#else
    if (last_ms != 0 && (now - last_ms) < 1 && !expander_hasInterrupt())
        return cached_state;
#endif
    last_ms = now;

#ifndef PICO_BOARD_TABLE
    uint16_t s = expander_readAll();   /* 16 bits, active LOW */
#endif
    unsigned char state = 0;

    /* Direct-GPIO controls */
#ifdef PICO_BOARD_TABLE
    if (pin_low(PIN_START))  state |= BUTTON_START;
    if (pin_low(PIN_SELECT)) state |= BUTTON_COIN;

    /* Player 1 joystick; fire also on the extra fire buttons / stick click.
     * Two-button games: the A buttons give FIRE, the B buttons FIRE2, and a
     * one-button joystick (no fireb) gives both. Otherwise all are FIRE. */
#ifdef PICO_DUO
    /* The Duo's 5-way is on the case, aligned for landscape: in portrait turn
     * it a quarter (right -> up, left -> down, up -> left — bench-checked). */
    if (!g_landscape_mode) {
        if (pin_low(g_pins.up))    state |= BUTTON_LEFT;
        if (pin_low(g_pins.down))  state |= BUTTON_RIGHT;
        if (pin_low(g_pins.left))  state |= BUTTON_DOWN;
        if (pin_low(g_pins.right)) state |= BUTTON_UP;
    } else
#endif
    {
    if (pin_low(g_pins.up))        state |= BUTTON_UP;
    if (pin_low(g_pins.down))      state |= BUTTON_DOWN;
    if (pin_low(g_pins.left))      state |= BUTTON_LEFT;
    if (pin_low(g_pins.right))     state |= BUTTON_RIGHT;
    }
#ifdef PICO_DUO
    /* Port 1 DB9 joystick too (fire 13 is g_pins.fire3). Not turned in
     * portrait: an external stick points whichever way the player holds it. */
    if (pin_low(DUO_DB9_UP))    state |= BUTTON_UP;
    if (pin_low(DUO_DB9_DOWN))  state |= BUTTON_DOWN;
    if (pin_low(DUO_DB9_LEFT))  state |= BUTTON_LEFT;
    if (pin_low(DUO_DB9_RIGHT)) state |= BUTTON_RIGHT;
#endif
    {
        const unsigned char B = g_two_fire ? BUTTON_FIRE2 : BUTTON_FIRE;
        const unsigned char joyFire = (g_two_fire && g_pins.fireb < 0) ? (BUTTON_FIRE | BUTTON_FIRE2)
                                                                       : BUTTON_FIRE;
        if (pin_low(g_pins.fire))   state |= joyFire;
        if (pin_low(g_pins.fire2))  state |= BUTTON_FIRE;
        if (pin_low(g_pins.fire3))  state |= BUTTON_FIRE;
        if (pin_low(g_pins.fire4))  state |= BUTTON_FIRE;
        if (pin_low(g_pins.fireb))  state |= B;
        if (pin_low(g_pins.fireb2)) state |= B;
        if (pin_low(g_pins.fireb3)) state |= B;
    }
    if (pin_low(g_pins.joy_click)) state |= BUTTON_FIRE;

    /* Atari 5200 controller stick (Duo 5200). The RC read blocks for up to
     * 2 ms, so it only runs on core 0 (the game list here; loop() during a
     * game) — core 1, the emulator, just uses the last result. */
    if (get_core_num() == 0) rc_stick_read();
    state |= s_stickDirs;
#ifdef PICO_USB_HOST
    /* USB pads / keyboard: polled on core 1 (pico_input_core1_usb_poll); the
     * serial monitor on core 0 (the game list here; update_screen in a game). */
    if (get_core_num() == 0) usb_poll();
    state |= s_usbState | serial_state();
#endif
#else
    if (digitalRead(PIN_START)  == LOW) state |= BUTTON_START;
    if (digitalRead(PIN_SELECT) == LOW) state |= BUTTON_COIN;

    /* Player 1 joystick — PCA9539 P0.0-P0.4. */
    if (!(s & EXP_P1_UP))    state |= BUTTON_UP;
    if (!(s & EXP_P1_DOWN))  state |= BUTTON_DOWN;
    if (!(s & EXP_P1_LEFT))  state |= BUTTON_LEFT;
    if (!(s & EXP_P1_RIGHT)) state |= BUTTON_RIGHT;
    /* external joystick: one button — both fires in a two-button game */
    if (!(s & EXP_P1_FIRE))  state |= g_two_fire ? (BUTTON_FIRE | BUTTON_FIRE2) : BUTTON_FIRE;

    /* On-board thumb buttons — both have external pull-ups on the PCB,
       so they're safe to poll. BTN_A duplicates FIRE (thumb-fire is
       handy for arcade play), BTN_B duplicates COIN — or is the second
       fire button (FIRE2) in a two-button game. */
    if (!(s & EXP_BTN_A))    state |= BUTTON_FIRE;
    if (!(s & EXP_BTN_B))    state |= g_two_fire ? BUTTON_FIRE2 : BUTTON_COIN;
#endif


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
#ifdef PICO_BOARD_TABLE
    if (PIN_JOY_X >= 0 && PIN_JOY_Y >= 0)
#endif
    {
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
    /* Press past JOY_THRESHOLD, let go below JOY_RELEASE (hysteresis). */
    static unsigned char joy_held = 0;
    const unsigned char J_L = 1, J_R = 2, J_U = 4, J_D = 8;
    if (jx < -((joy_held & J_L) ? JOY_RELEASE : JOY_THRESHOLD)) joy_held |= J_L; else joy_held &= ~J_L;
    if (jx >  ((joy_held & J_R) ? JOY_RELEASE : JOY_THRESHOLD)) joy_held |= J_R; else joy_held &= ~J_R;
    if (jy < -((joy_held & J_U) ? JOY_RELEASE : JOY_THRESHOLD)) joy_held |= J_U; else joy_held &= ~J_U;
    if (jy >  ((joy_held & J_D) ? JOY_RELEASE : JOY_THRESHOLD)) joy_held |= J_D; else joy_held &= ~J_D;
    if (joy_held & J_L) state |= BUTTON_LEFT;
    if (joy_held & J_R) state |= BUTTON_RIGHT;
    if (joy_held & J_U) state |= BUTTON_UP;
    if (joy_held & J_D) state |= BUTTON_DOWN;
    }

    /* MENU: a tap is reported on release (the game list uses it to exit);
     * held during a game it returns to the game list. A press that is
     * already down at boot (the hold that just relaunched us, or leaving
     * ROM drive mode) is ignored until released, so the late release
     * doesn't also exit the list. */
    {
        static bool menu_down = false, seen_up = false;
        static unsigned long menu_since = 0;
#ifdef PICO_BOARD_TABLE
        bool menu_low = pin_low(PIN_MENU);
#else
        bool menu_low = digitalRead(PIN_MENU) == LOW;
#endif
        if (!menu_low) seen_up = true;
        if (menu_low && seen_up) {
            if (!menu_down) { menu_down = true; menu_since = now; }
            if (g_menu_done && now - menu_since >= MENU_EXIT_HOLD_MS)
                pico_hiscore_exit_to_list();     /* saves a changed high-score table first */
        } else if (menu_down) {
            menu_down = false;
            g_menu_tapped = true;
#ifdef INGAME_WIDTH_TAP
            /* In a game, landscape: a tap steps the width 62 -> 62 crop -> 75 -> ... -> 160 crop
             * (Quad / Duo 5200: 186 ... 320 crop); the
             * video side picks it up next frame and saves it (video.cpp). */
            if (g_menu_done && g_landscape_mode) g_stretch = (g_stretch + 1) % 8;
#endif
        }
    }

#ifdef PICO_BOARD_TABLE
    /* Menu-only UP/DOWN buttons (Quad: left / right side buttons) — step
     * through the game list; not passed to the game. */
    if (!g_menu_done) {
        if (pin_low(g_pins.nav_up))   state |= BUTTON_UP;
        if (pin_low(g_pins.nav_down)) state |= BUTTON_DOWN;
    }
#endif

    /* Hold START+SELECT during a game → back to the game list. */
    {
        static unsigned long held_since = 0;
        if (g_menu_done && (state & BUTTON_START) && (state & BUTTON_COIN)) {
            if (held_since == 0) held_since = now | 1;
            else if (now - held_since >= EXIT_HOLD_MS) pico_hiscore_exit_to_list();
        } else {
            held_since = 0;
        }
    }

    if (g_menu_done && four_way_game()) state = four_way(state);

    cached_state = state;
#ifdef Z80_BENCH
    return 0;      /* benchmark: no input, so every run is identical */
#endif
    return state;
}
