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
 *                 Duo profile, no ARCADE.INI. Without PICO_STANDALONE: chain-
 *                 booted by the Duo firmware (2.3+) from its arcade slot.
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
#if defined(PICO_QUAD) || defined(PICO_5200) || (defined(PICO_DUO) && !defined(PICO_STANDALONE))
#  define PICO_HOSTED 1
#endif
#if defined(PICO_HOSTED) || defined(PICO_STANDALONE)
#  define PICO_BOARD_TABLE 1
#endif
/* Duo Ultimate port 1 DB9 directions (data/duoultimate.json), read beside
 * the 5-way (g_pins has one pin per direction); its fire is g_pins.fire3. */
#ifdef PICO_DUO
#  define DUO_DB9_UP    16
#  define DUO_DB9_DOWN  17
#  define DUO_DB9_LEFT  18
#  define DUO_DB9_RIGHT 19
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

/* Landscape scale modes (menu Scale row; builds with
 * PICO_GALAGINO_LANDSCAPE_AREA — video.cpp g_scale_mode). */
#define SCALE_SMOOTH 0   /* area average (default) */
#define SCALE_SHARP  1   /* copy 4 of 5, blend the middle pair */
#define SCALE_DROP   2   /* drop every 6th row / column */

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

/* Fruit Jam: the emulator's per-frame code and the sound path in RAM. With
 * the game's sound running on core 0 (I2S), the emulator on core 1 went from
 * 60 fps with room to spare to ~48 fps (20 ms a frame): both cores' code
 * from flash competing for the XIP cache they share (as in Pico2Atari8, whose
 * hot paths are in RAM for the same reason - its config.h A8_RAM). Other
 * boards: unchanged (in flash). */
#ifdef PICO_FRUITJAM
#  define PICO_HOT_RAM(f) __not_in_flash_func(f)
#else
#  define PICO_HOT_RAM(f) f
#endif

#ifdef PICO_FRUITJAM
#include <stddef.h>
/* Erase the 4 KB sector at flash offset ofs and program len bytes into it,
 * then put the flash's read settings back (pico_config.cpp: the SDK leaves
 * XIP in the boot ROM's slow mode on the RP2350). Interrupts off, the other
 * core parked, as around flash_range_erase / _program. */
void pico_flash_erase_program(uint32_t ofs, const uint8_t *data, size_t len);
#endif
#ifdef PICO_USB_HOST
/* After a flash write (pico_input.cpp): power-cycle the USB ports. */
void pico_input_flash_written(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
