/* config.h — PicoGalagino port config for RP2350B / PicoCartReader hardware.
 * Replaces upstream's ESP32-specific config. Pin mappings match the PicoCartReader
 * PCB so we can share hardware between the VCS emulator and this Galagino port. */

#ifndef _CONFIG_H_
#define _CONFIG_H_

// -----------------------------------------------------------------------------
// Game enable — start with just Pac-Man to keep the initial port small.
// Add more games one at a time by defining ENABLE_GALAGA etc.
// -----------------------------------------------------------------------------
#if defined(GAL_ONLY_PACMAN)
#  define ENABLE_PACMAN
#elif defined(GAL_ONLY_GALAGA)
#  define ENABLE_GALAGA
#else
/* Multi-machine mode: enable only games with runtime loaders. Add each
 * new game here as its loader lands (DKong, Digdug, 1942). */
#  define ENABLE_PACMAN
#  define ENABLE_MSPACMAN
#  define ENABLE_GALAGA
#  define ENABLE_FROGGER
#  define ENABLE_DKONG
#  define ENABLE_DIGDUG
#  define ENABLE_INVADERS
#  define ENABLE_SCOBRA
#endif

#if !defined(ENABLE_PACMAN) && !defined(ENABLE_GALAGA) && !defined(ENABLE_DKONG) \
 && !defined(ENABLE_FROGGER) && !defined(ENABLE_DIGDUG) && !defined(ENABLE_1942)
#  error "At least one machine has to be enabled!"
#endif

// Single-machine flag lets upstream elide the menu.
#if (( defined(ENABLE_PACMAN) && !defined(ENABLE_GALAGA) && !defined(ENABLE_DKONG) && !defined(ENABLE_FROGGER) && !defined(ENABLE_DIGDUG) && !defined(ENABLE_1942)) || \
     (!defined(ENABLE_PACMAN) &&  defined(ENABLE_GALAGA) && !defined(ENABLE_DKONG) && !defined(ENABLE_FROGGER) && !defined(ENABLE_DIGDUG) && !defined(ENABLE_1942)) || \
     (!defined(ENABLE_PACMAN) && !defined(ENABLE_GALAGA) &&  defined(ENABLE_DKONG) && !defined(ENABLE_FROGGER) && !defined(ENABLE_DIGDUG) && !defined(ENABLE_1942)) || \
     (!defined(ENABLE_PACMAN) && !defined(ENABLE_GALAGA) && !defined(ENABLE_DKONG) &&  defined(ENABLE_FROGGER) && !defined(ENABLE_DIGDUG) && !defined(ENABLE_1942)) || \
     (!defined(ENABLE_PACMAN) && !defined(ENABLE_GALAGA) && !defined(ENABLE_DKONG) && !defined(ENABLE_FROGGER) &&  defined(ENABLE_DIGDUG) && !defined(ENABLE_1942)) || \
     (!defined(ENABLE_PACMAN) && !defined(ENABLE_GALAGA) && !defined(ENABLE_DKONG) && !defined(ENABLE_FROGGER) && !defined(ENABLE_DIGDUG) &&  defined(ENABLE_1942)))
#  define SINGLE_MACHINE
#endif

// -----------------------------------------------------------------------------
// Game runtime tunables (upstream defaults)
// -----------------------------------------------------------------------------
// Galagino's own logo menu (machine == MCH_MENU) is never shown here:
// pico_menu.cpp picks the game before emulation starts. Compile it out so
// its logo images (~68 KB of flash) aren't linked in.
#define PICO_NO_UPSTREAM_MENU

#define MASTER_ATTRACT_MENU_TIMEOUT  20000
#include "dip_switches.h"

// -----------------------------------------------------------------------------
// Display — ST7789 240x320. TFT_eSPI settings live in platformio.ini,
// so we ignore upstream's ESP-IDF SPI pin macros here. The offsets stay
// (game uses a 224x288 window centered in 240x320).
// -----------------------------------------------------------------------------
#define TFT_X_OFFSET   8
#define TFT_Y_OFFSET  16
// Leave TFT_SPICLK undefined so Galagino picks VIDEO_HALF_RATE mode.
// In that mode the render loop sends TWO xTaskNotifyGive per screen
// refresh, so the emulator ticks at 2× display rate — game clock advances
// faster than the display can keep up, but that's better than 1:1 sync
// which pins the emulator to display fps (feels like ~30% speed).

// -----------------------------------------------------------------------------
// Audio — placeholder. Real path lives in pico_glue/audio.cpp (PWM on GP22
// via the same amp gate we use for VCS). For now, muted.
// -----------------------------------------------------------------------------
#define MASTER_VOLUME  64

// -----------------------------------------------------------------------------
// Input — PicoCartReader-style. Same PCA9539 expander + direct GPIO buttons
// used by the VCS emulator. Actual wiring lives in pico_glue/input.cpp.
// The BTN_*_PIN macros below are placeholders; the glue reads state via
// its own inline helpers and updates the io_state[] array upstream reads.
// -----------------------------------------------------------------------------
#define BTN_START_PIN  0     // GP0 = START button (direct GPIO)
#define BTN_COIN_PIN   1     // GP1 = SELECT = "insert coin"
#define BTN_LEFT_PIN   100   // sentinel — real reads happen via pico_input.cpp
#define BTN_RIGHT_PIN  101
#define BTN_DOWN_PIN   102
#define BTN_UP_PIN     103
#define BTN_FIRE_PIN   104

/* Route buttons_get() through pico_input.cpp (PCA9539 + direct GPIO)
 * instead of the built-in digitalRead(BTN_*_PIN) path — those pin
 * numbers reference ESP32 GPIOs that don't exist on our board. */
#define PICO_CUSTOM_INPUT 1

// LED strip disabled on this hardware (no WS2812).
// (Do NOT define LED_PIN.)

#endif // _CONFIG_H_
