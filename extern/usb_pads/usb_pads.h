/* usb_pads.h - USB gamepads and a keyboard on an RP2350's PIO-USB host port
 * (the Adafruit Fruit Jam's USB-A ports): TinyUSB's host stack over
 * Pico-PIO-USB, any HID gamepad / joystick read by its report descriptor.
 *
 * Shared, unchanged, by Pico2Atari8 and Pico2Arcade - no emulator in here.
 * Don't write another HID parser: fix it here, for both.
 * (docs/FRUITJAM_PORTING.md in Pico2Atari8: how to build and call it.)
 *
 * Needs: Adafruit TinyUSB (-DUSE_TINYUSB), Pico-PIO-USB (extern/pico-pio-usb
 * on the include path; its .c files and usb_pads_glue.c compiled once, in a
 * C file), TinyUSB's enumeration buffer raised to 1024
 * (extern/tinyusb-config). All calls from ONE core - the one that called
 * usb_pads_begin (its 1 ms frame timer runs there, at the highest IRQ
 * priority): not the core with the HDMI scanline interrupt.
 *
 * Pads are numbered as they connect: 0 (player 1), 1 (player 2). A pad is
 * an interface with an X axis or a hat inside a Joystick / GamePad
 * collection (or the Adafruit SNES-style pad, 081f:e401). */
#ifndef USB_PADS_H
#define USB_PADS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A pad's buttons and directions (usb_pads_buttons / usb_pads_stick_dirs).
 * Generic pads' HID buttons by number: 1 B (the main one - a joystick's
 * trigger), 2 A, 3 X, 4 Y, 5 / 7 L, 6 / 8 R, 9 SELECT, 10 START. */
enum {
    USBP_UP = 1, USBP_DOWN = 2, USBP_LEFT = 4, USBP_RIGHT = 8,
    USBP_A = 16, USBP_B = 32, USBP_X = 64, USBP_Y = 128, USBP_L = 256, USBP_R = 512,
    USBP_SELECT = 1024, USBP_START = 2048
};

typedef struct {
    int dp_pin;              /* D+ (D- = D+ + 1) */
    int power_pin;           /* the ports' 5 V switch; -1: none */
    int power_on;            /* its level for on */
    int pio;                 /* PIO block for PIO-USB (clear of HDMI / I2S) */
    int dma;                 /* DMA channel for PIO-USB */
} usb_pads_config_t;

/* The Fruit Jam: D+ GPIO 1, 5 V on GPIO 11 (high), PIO 2, DMA channel 11. */
#define USB_PADS_FRUITJAM { 1, 11, 1, 2, 11 }

/* Start the host (once; on the core that will call everything else). */
void usb_pads_begin(const usb_pads_config_t *cfg);
/* The host stack's task: call often (every input poll - every frame and
 * every menu loop; a screen with its own loop too). Also runs the unplug
 * watchdog and the power-cycle below. */
void usb_pads_task(void);
/* Ports' 5 V off for 300 ms, then on: every device is set up again. Call
 * after anything that ran with interrupts off for more than ~3 ms (a flash
 * write): the devices suspended meanwhile and some never wake. */
void usb_pads_power_cycle(void);

/* Pad n (0, 1). */
bool     usb_pads_connected(int n);
uint32_t usb_pads_buttons(int n);      /* USBP_*: buttons and D-pad / hat */
uint32_t usb_pads_stick_dirs(int n);   /* USBP_* directions of its X / Y axes (past a quarter) */
uint16_t usb_pads_numbers(int n);      /* HID buttons 1-16 held (bit n - 1; not the SNES pad) */
bool     usb_pads_analog(int n);       /* it has an analog X axis (by its descriptor) */
/* Axis 0 X, 1 Y as -1000 .. +1000 of the range its descriptor gives (0:
 * centre; + right / down); 0 if none. */
int      usb_pads_axis(int n, int axis);
/* The same axis as it came: its value and its descriptor's logical range
 * (a test screen; 0s if none). */
void     usb_pads_axis_raw(int n, int axis, int32_t *value, int32_t *lmin, int32_t *lmax);
void     usb_pads_ids(int n, uint16_t *vid, uint16_t *pid);
/* Its last report as it came (up to 16 bytes); returns the length. */
int      usb_pads_raw(int n, uint8_t raw[16]);

/* The keyboard (boot protocol). */
bool           usb_pads_keyboard(void);
const uint8_t *usb_pads_kbd_report(void);   /* 8 bytes: modifiers, 0, 6 key usages */
bool           usb_pads_kbd_has(uint8_t usage);   /* that key held */

/* Counts mounts / unmounts: changed = something connected or left. */
uint32_t usb_pads_events(void);

#ifdef __cplusplus
}
#endif

#endif
