/* pico_board.h — runtime pin map for the table-driven boards.
 *
 * PICO_QUAD       Quad Ultimate CX (chain-booted): built-in Quad profile.
 * PICO_STANDALONE plain Pico 2 dev board: defaults below, overridden by
 *                 ARCADE.INI in the root of the ROM drive (written with the
 *                 defaults, commented, when the drive is first formatted).
 * (PicoCartReader keeps its fixed PCA9539 expander wiring — not table-driven.)
 *
 * All buttons/directions are active LOW with the internal pull-up (switch to
 * GND). -1 = not connected. The display pins are still compile-time
 * (TFT_eSPI build flags).
 */
#ifndef PICO_BOARD_H
#define PICO_BOARD_H

#include <stdint.h>

#if defined(PICO_QUAD) || defined(PICO_STANDALONE)
#  define PICO_BOARD_TABLE 1
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
    int8_t fire, fire2, fire3;      /* any of them = FIRE */
    int8_t start, coin, menu;
    int8_t joy_x, joy_y;            /* ADC GPIOs for an analog stick, -1 = none */
    int8_t joy_click;               /* = FIRE */
    int8_t audio;                   /* PWM audio output */
    int8_t amp;                     /* amplifier enable (active HIGH), -1 = none */
    int8_t backlight;               /* display backlight (driven HIGH), -1 = none */
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
