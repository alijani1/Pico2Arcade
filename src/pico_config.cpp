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

#ifdef PICO_QUAD
/* Quad Ultimate CX: no SD card and the flash ROM drive is read-only to us.
 * The display orientation is kept by the Quad instead: it passes it in the
 * handoff flags (watchdog scratch[1], HANDOFF_FLAG_PORTRAIT) on launch, we
 * write it back when it changes (the menu then relaunches us), and the Quad
 * saves it in its settings on return. Default landscape (the panel is mounted
 * that way; portrait = turn the Quad sideways for a full-size picture). The FPS
 * overlay is session-only, off at boot. */
#include "pico_handoff.h"
extern "C" {
    volatile int g_landscape_mode = 1;
    volatile int g_show_fps       = 0;
}
extern "C" void pico_config_load(void) {
    int f = handoff_flags_read();
    if (f >= 0) g_landscape_mode = (f & HANDOFF_FLAG_PORTRAIT) ? 0 : 1;
    Serial.printf("[cfg] landscape=%d (from Quad)\n", g_landscape_mode);
}
extern "C" void pico_config_save_landscape(int on) {
    g_landscape_mode = on ? 1 : 0;
    int f = handoff_flags_read();
    if (f < 0) f = 0;
    f = on ? (f & ~HANDOFF_FLAG_PORTRAIT) : (f | HANDOFF_FLAG_PORTRAIT);
    handoff_flags_write(f);
}
extern "C" void pico_config_save_show_fps(int on)  { g_show_fps = on ? 1 : 0; }
extern "C" int  pico_config_volume(void) { return -1; }        /* the Quad keeps it */
extern "C" void pico_config_save_volume(int pct) { (void)pct; }
#elif defined(PICO_STANDALONE)
/* Plain Pico 2: no SD card — settings live in one 4 KB flash sector just
 * above the 1 MB ROM drive (the firmware is far below). Only written from
 * the menu on core 0 (core 1 is still parked in setup1 then). Default:
 * portrait (full-size picture on a 240x320 panel), FPS off, volume 100%. */
#include <hardware/flash.h>
#include <hardware/sync.h>
#include "pico_audio.h"

#ifndef PICO_CONFIG_PHYS
#define PICO_CONFIG_PHYS 0x300000u
#endif
static const uint32_t CFG_MAGIC = 0x47434647u;          /* "GCFG" */
struct Cfg { uint32_t magic; uint8_t landscape, show_fps, volume, pad; };
static Cfg s_cfg = { CFG_MAGIC, 0, 0, 100, 0 };

extern "C" {
    volatile int g_landscape_mode = 0;   /* default: portrait */
    volatile int g_show_fps       = 0;   /* default: off */
}

extern "C" void pico_config_load(void) {
    const Cfg *f = (const Cfg *)(XIP_BASE + PICO_CONFIG_PHYS);
    if (f->magic == CFG_MAGIC) s_cfg = *f;
    if (s_cfg.volume > 100) s_cfg.volume = 100;
    g_landscape_mode = s_cfg.landscape ? 1 : 0;
    g_show_fps = s_cfg.show_fps ? 1 : 0;
    Serial.printf("[cfg] landscape=%d show_fps=%d volume=%d (%s)\n", g_landscape_mode,
                  g_show_fps, s_cfg.volume, f->magic == CFG_MAGIC ? "flash" : "defaults");
}

static void pico_config_save(void) {
    s_cfg.magic = CFG_MAGIC;
    s_cfg.landscape = g_landscape_mode ? 1 : 0;
    s_cfg.show_fps = g_show_fps ? 1 : 0;
    const Cfg *f = (const Cfg *)(XIP_BASE + PICO_CONFIG_PHYS);
    if (memcmp(f, &s_cfg, sizeof(s_cfg)) == 0) return;          /* unchanged */
    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, &s_cfg, sizeof(s_cfg));
    rp2040.idleOtherCore();
    pico_audio_suspend();                    /* PWM audio DMA can't miss IRQs (pico_audio.cpp) */
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(PICO_CONFIG_PHYS, FLASH_SECTOR_SIZE);
    flash_range_program(PICO_CONFIG_PHYS, page, FLASH_PAGE_SIZE);
    restore_interrupts(irq);
    pico_audio_resume();
    rp2040.resumeOtherCore();
    Serial.printf("[cfg] saved landscape=%d show_fps=%d volume=%d\n",
                  s_cfg.landscape, s_cfg.show_fps, s_cfg.volume);
}

extern "C" void pico_config_save_landscape(int on) {
    g_landscape_mode = on ? 1 : 0;
    pico_config_save();
}
extern "C" void pico_config_save_show_fps(int on) {
    g_show_fps = on ? 1 : 0;
    pico_config_save();
}
/* Stored volume (0..100) — the menu uses it when no handoff volume is set. */
extern "C" int pico_config_volume(void) { return s_cfg.volume; }
extern "C" void pico_config_save_volume(int pct) {
    if (pct < 0 || pct > 100 || pct == s_cfg.volume) return;
    s_cfg.volume = (uint8_t)pct;
    pico_config_save();
}
#else
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
/* The CartReader keeps the volume itself (handoff). */
extern "C" int  pico_config_volume(void) { return -1; }
extern "C" void pico_config_save_volume(int pct) { (void)pct; }
#endif  /* !PICO_QUAD */
