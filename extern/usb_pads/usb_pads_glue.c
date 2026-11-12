/* usb_pads_glue.c - the C side of usb_pads (see usb_pads.h): Pico-PIO-USB's
 * sources and TinyUSB's HID mount / unmount callbacks. Compile it once, as
 * C (Pico2Atari8: #included by src/a8_stubs.c; elsewhere a file of its own).
 * Needs extern/pico-pio-usb on the include path.
 *
 * The callbacks are defined here, not in usb_pads.cpp: TinyUSB's header
 * declares them weak, which made definitions in a file including it weak
 * too, and the linker took Adafruit's empty ones - the pad was set up but
 * never asked for a report. This file doesn't include that header. */
#include <stdint.h>
#include "pio_usb.c"
#include "pio_usb_host.c"
#include "pio_usb_device.c"
#include "usb_crc.c"

void usb_pads_hid_mount(uint8_t dev, uint8_t idx, uint8_t const *desc, uint16_t desc_len);
void usb_pads_hid_umount(uint8_t dev, uint8_t idx);
void tuh_hid_mount_cb(uint8_t dev, uint8_t idx, uint8_t const *desc, uint16_t desc_len) {
    usb_pads_hid_mount(dev, idx, desc, desc_len);
}
void tuh_hid_umount_cb(uint8_t dev, uint8_t idx) { usb_pads_hid_umount(dev, idx); }
