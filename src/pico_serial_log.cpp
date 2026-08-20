/* pico_serial_log.cpp — C-linkage wrapper so plain C files can log
 * to the USB CDC Serial that PIO's monitor is attached to. printf()
 * routes to UART in the earlephilhower framework and doesn't reach
 * the monitor. */

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

extern "C" void pico_serial_log(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print(buf);
}
