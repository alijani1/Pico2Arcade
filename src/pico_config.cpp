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
#include "pico_board.h"      /* PICO_HOSTED */

#ifdef PICO_HOSTED
/* Quad Ultimate CX: no SD card and the flash ROM drive is read-only to us.
 * The display orientation is kept by the Quad instead: it passes it in the
 * handoff flags (watchdog scratch[1], HANDOFF_FLAG_PORTRAIT) on launch, we
 * write it back when it changes (the menu then relaunches us), and the Quad
 * saves it in its settings on return. Default landscape (the panel is mounted
 * that way; portrait = turn the Quad sideways for a full-size picture). The FPS
 * overlay and the landscape Scale ride in the same flags (HANDOFF_FLAG_SHOW_FPS,
 * _SCALE_*): kept across our relaunches, and across a power-off once the host
 * saves the whole flags byte — until then they are back to off / Smooth. */
#include "pico_handoff.h"
extern "C" {
    volatile int g_landscape_mode = 1;
    volatile int g_show_fps       = 0;
}
#ifdef PICO_GALAGINO_LANDSCAPE_AREA
extern "C" volatile int g_scale_mode;    /* video.cpp */
#endif
static void flags_update(int clear, int set) {
    int f = handoff_flags_read();
    if (f < 0) f = 0;
    handoff_flags_write((f & ~clear) | set);
}
extern "C" void pico_config_load(void) {
    int f = handoff_flags_read();
    if (f >= 0) {
        g_landscape_mode = (f & HANDOFF_FLAG_PORTRAIT) ? 0 : 1;
        g_show_fps = (f & HANDOFF_FLAG_SHOW_FPS) ? 1 : 0;
#ifdef PICO_GALAGINO_LANDSCAPE_AREA
        int sc = (f & HANDOFF_FLAG_SCALE_MASK) >> HANDOFF_FLAG_SCALE_SHIFT;
        g_scale_mode = sc <= SCALE_DROP ? sc : SCALE_SMOOTH;
#endif
    }
    Serial.printf("[cfg] landscape=%d show_fps=%d (from Quad)\n", g_landscape_mode, g_show_fps);
}
extern "C" void pico_config_save_landscape(int on) {
    g_landscape_mode = on ? 1 : 0;
    int f = handoff_flags_read();
    if (f < 0) f = 0;
    f = on ? (f & ~HANDOFF_FLAG_PORTRAIT) : (f | HANDOFF_FLAG_PORTRAIT);
    handoff_flags_write(f);
}
extern "C" void pico_config_save_show_fps(int on) {
    g_show_fps = on ? 1 : 0;
    flags_update(HANDOFF_FLAG_SHOW_FPS, on ? HANDOFF_FLAG_SHOW_FPS : 0);
}
extern "C" void pico_config_save_scale(int mode) {
    flags_update(HANDOFF_FLAG_SCALE_MASK, (mode << HANDOFF_FLAG_SCALE_SHIFT) & HANDOFF_FLAG_SCALE_MASK);
}
extern "C" int  pico_config_volume(void) { return -1; }        /* the Quad keeps it */
extern "C" void pico_config_save_volume(int pct) { (void)pct; }
#elif defined(PICO_STANDALONE)
/* Plain Pico 2: no SD card — settings live in one 4 KB flash sector just
 * above the 1 MB ROM drive (the firmware is far below). Only written from
 * the menu on core 0 (core 1 is still parked in setup1 then). Default:
 * portrait (full-size picture on a 240x320 panel), FPS off, volume 50%. */
#include <hardware/flash.h>
#include <hardware/sync.h>
#include "pico_audio.h"

#ifndef PICO_CONFIG_PHYS
#define PICO_CONFIG_PHYS 0x300000u
#endif
static const uint32_t CFG_MAGIC = 0x47434647u;          /* "GCFG" */
/* scale: the 160x80 panel's Scale row (SCALE_*; was padding, so 0 = Smooth) */
/* stretch: the 160x80 panel's landscape Width (0-7 = 62, 62 crop, 75, 75 crop,
 * 112, 112 crop, 160, 160 crop); older
 * settings end before it, so it reads the erased 0xFF there = 62 */
struct Cfg { uint32_t magic; uint8_t landscape, show_fps, volume, scale, stretch, pad[3]; };
/* The Duo's 160x80 panel starts in landscape, the way the Duo is held. */
#ifdef PICO_TINY_SCREEN
#define PICO_DEFAULT_LANDSCAPE 1
#else
#define PICO_DEFAULT_LANDSCAPE 0
#endif
static Cfg s_cfg = { CFG_MAGIC, PICO_DEFAULT_LANDSCAPE, 0, 50, 0 };

#ifdef PICO_TINY_SCREEN
extern "C" volatile int g_scale_mode;    /* video.cpp */
extern "C" volatile int g_stretch;       /* video.cpp */
#endif
extern "C" {
    volatile int g_landscape_mode = PICO_DEFAULT_LANDSCAPE;   /* default: portrait */
    volatile int g_show_fps       = 0;   /* default: off */
}

extern "C" void pico_config_load(void) {
    const Cfg *f = (const Cfg *)(XIP_BASE + PICO_CONFIG_PHYS);
    if (f->magic == CFG_MAGIC) s_cfg = *f;
    if (s_cfg.volume > 100) s_cfg.volume = 100;
    g_landscape_mode = s_cfg.landscape ? 1 : 0;
    g_show_fps = s_cfg.show_fps ? 1 : 0;
#ifdef PICO_TINY_SCREEN
    g_scale_mode = s_cfg.scale <= SCALE_DROP ? s_cfg.scale : SCALE_SMOOTH;
    if (s_cfg.stretch > 7) s_cfg.stretch = 0;
    g_stretch = s_cfg.stretch;
#endif
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
#ifdef PICO_TINY_SCREEN
extern "C" void pico_config_save_scale(int mode) {
    s_cfg.scale = (uint8_t)mode;
    pico_config_save();
}
extern "C" void pico_config_save_stretch(int idx) {
    g_stretch = (idx >= 0 && idx <= 7) ? idx : 0;
    s_cfg.stretch = (uint8_t)g_stretch;
    pico_config_save();
}
#endif
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
#endif  /* !PICO_HOSTED */
