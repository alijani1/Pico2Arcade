/* pico_board.h — runtime pin map for the table-driven boards.
 *
 * PICO_QUAD       Quad Ultimate CX (chain-booted): built-in Quad profile.
 * PICO_5200       Duo 5200 Ultimate (chain-booted): built-in 5200 profile —
 *                 Joy-Con + device buttons + the port 1 Atari 5200 controller
 *                 (analog stick read by RC charge time, Fire A/B).
 * PICO_STANDALONE plain Pico 2 dev board: defaults below, overridden by
 *                 ARCADE.INI in the root of the ROM drive (written with the
 *                 defaults, commented, when the drive is first formatted).
 * PICO_DUO        (with PICO_STANDALONE) Duo Ultimate run standalone, as a
 *                 test of the 160x80 ST7735 panel (PICO_TINY_SCREEN): built-in
 *                 Duo profile, no ARCADE.INI.
 * (PicoCartReader keeps its fixed PCA9539 expander wiring — not table-driven.)
 *
 * All buttons/directions are active LOW with the internal pull-up (switch to
 * GND). -1 = not connected. The display pins are still compile-time
 * (TFT_eSPI build flags).
 */
#ifndef PICO_BOARD_H
#define PICO_BOARD_H

#include <stdint.h>

/* Chain-booted by an iCode host firmware (GameAdapterRGBPico include/arcade.hpp):
 * the host passes volume/orientation and the ROM drive location, and gets
 * them back on exit. */
#if defined(PICO_QUAD) || defined(PICO_5200)
#  define PICO_HOSTED 1
#endif
#if defined(PICO_HOSTED) || defined(PICO_STANDALONE)
#  define PICO_BOARD_TABLE 1
#endif
/* Panel rotations for the two display modes. The 240x320 boards: landscape
 * 3, portrait 0. The Duo's 160x80 panel reads landscape at 1 (as in the Duo
 * firmware); portrait is 2 — one quarter turn on, the same way 3 -> 0 is on
 * the other boards, so the Joy-Con / 5-way portrait remap is the same. */
#ifdef PICO_TINY_SCREEN
#  define PICO_ROT_LANDSCAPE 1
#  define PICO_ROT_PORTRAIT  2
#else
#  define PICO_ROT_LANDSCAPE 3
#  define PICO_ROT_PORTRAIT  0
#endif

/* USB ROM drive mode (pico_drive.cpp) — builds with the TinyUSB stack. */
#if defined(PICO_BOARD_TABLE) && defined(USE_TINYUSB)
#  define PICO_USB_DRIVE 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int8_t up, down, left, right;
    int8_t fire, fire2, fire3, fire4;   /* A: any of them = FIRE */
    int8_t fireb, fireb2, fireb3;       /* B: FIRE2 in two-button games, else FIRE.
                                           fireb = the joystick's own second button;
                                           without it, fire gives both A and B there */
    int8_t start, coin, menu;
    int8_t nav_up, nav_down;        /* menu-only UP / DOWN buttons, -1 = none */
    int8_t joy_x, joy_y;            /* ADC GPIOs for an analog stick, -1 = none */
    int8_t joy_click;               /* = FIRE */
    int8_t audio;                   /* PWM audio output */
    int8_t amp;                     /* amplifier enable (active HIGH), -1 = none */
    int8_t backlight;               /* display backlight (driven HIGH), -1 = none */
    int8_t stick_x, stick_y;        /* Atari RC analog stick (5200 controller pots), -1 = none */
} board_pins_t;

extern board_pins_t g_pins;

/* Fill g_pins: board profile, then (standalone) ARCADE.INI. Call after
 * romfs_init() and before audio / input init. */
void board_pins_load(void);
/* Contents of the default ARCADE.INI (standalone), for the drive formatter. */
const char *board_default_ini(void);
/* Non-empty if ARCADE.INI had problems (shown once in the menu). */
const char *board_pins_warning(void);

#ifdef __cplusplus
}
#endif

#endif
