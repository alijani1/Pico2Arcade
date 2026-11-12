/* tusb_config_fruitjam.h - TinyUSB's configuration for the Fruit Jam build
 * (platformio.ini: CFG_TUSB_CONFIG_FILE): Adafruit TinyUSB's own, with one
 * change - the enumeration buffer 1024 bytes, not 256. It holds a device's
 * configuration descriptor (a device whose interfaces together are longer
 * is not set up at all) and each HID report descriptor (a longer one is
 * handed over empty: 0e8f:3013's second interface - keyboard, mouse and
 * vendor collections - is 263 bytes). 768 bytes more RAM. */
#ifndef TUSB_CONFIG_FRUITJAM_H_
#define TUSB_CONFIG_FRUITJAM_H_

#include "tusb_config.h"                   /* Adafruit TinyUSB's (arduino/ports/rp2040) */

#undef  CFG_TUH_ENUMERATION_BUFSIZE
#define CFG_TUH_ENUMERATION_BUFSIZE 1024

#endif
