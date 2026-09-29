/* pico_config.cpp — persistent settings on SD (/gal.cfg).
 *
 * Byte 0: '0' portrait / '1' landscape.
 * Byte 1: '0' / '1' show the FPS overlay (missing = off, so older
 *         one-byte files still load).
 * Loaded once at boot BEFORE Video::begin() runs so the video code can
 * pick rotation + downscale path. The menu writes it; the orientation
 * toggle soft-reboots, the FPS toggle takes effect immediately.
 * Default (no file present) is portrait, FPS overlay off. */

#include <Arduino.h>
#include <SD.h>

extern "C" {
    volatile int g_landscape_mode = 0;   /* default: portrait */
    volatile int g_show_fps       = 0;   /* default: off */
}

extern "C" void pico_config_load(void) {
    File f = SD.open("/gal.cfg", FILE_READ);
    if (!f) return;
    int b   = f.read();
    int fps = f.read();                  /* -1 on older one-byte files */
    f.close();
    if (b == '1') g_landscape_mode = 1;
    else if (b == '0') g_landscape_mode = 0;
    g_show_fps = (fps == '1');
    Serial.printf("[cfg] landscape=%d show_fps=%d (from /gal.cfg)\n",
                  g_landscape_mode, g_show_fps);
}

static void pico_config_save(void) {
    SD.remove("/gal.cfg");
    File f = SD.open("/gal.cfg", FILE_WRITE);
    if (!f) { Serial.println("[cfg] SD write failed"); return; }
    f.write((uint8_t)(g_landscape_mode ? '1' : '0'));
    f.write((uint8_t)(g_show_fps ? '1' : '0'));
    f.close();
    Serial.printf("[cfg] saved landscape=%d show_fps=%d\n",
                  g_landscape_mode, g_show_fps);
}

extern "C" void pico_config_save_landscape(int on) {
    g_landscape_mode = on ? 1 : 0;
    pico_config_save();
}

extern "C" void pico_config_save_show_fps(int on) {
    g_show_fps = on ? 1 : 0;
    pico_config_save();
}
