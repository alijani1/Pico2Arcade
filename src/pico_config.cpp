/* pico_config.cpp — persistent orientation setting on SD.
 *
 * Single-byte file /gal.cfg holds '0' (portrait) or '1' (landscape).
 * Loaded once at boot BEFORE Video::begin() runs so the video code can
 * pick rotation + downscale path. Menu writes it and soft-reboots on
 * toggle. Default (no file present) is portrait. */

#include <Arduino.h>
#include <SD.h>

extern "C" {
    volatile int g_landscape_mode = 0;   /* default: portrait */
}

extern "C" void pico_config_load(void) {
    File f = SD.open("/gal.cfg", FILE_READ);
    if (!f) return;
    int b = f.read();
    f.close();
    if (b == '1') g_landscape_mode = 1;
    else if (b == '0') g_landscape_mode = 0;
    Serial.printf("[cfg] landscape=%d (from /gal.cfg)\n", g_landscape_mode);
}

extern "C" void pico_config_save_landscape(int on) {
    SD.remove("/gal.cfg");
    File f = SD.open("/gal.cfg", FILE_WRITE);
    if (!f) { Serial.println("[cfg] SD write failed"); return; }
    f.write((uint8_t)(on ? '1' : '0'));
    f.close();
    Serial.printf("[cfg] saved landscape=%d\n", on ? 1 : 0);
}
