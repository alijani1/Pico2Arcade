/* video.cpp — Pico2Arcade: TFT_eSPI backend for the Video class.
 *
 * Galagino streams the game frame as 36 sequential 224x8-pixel strips
 * top-to-bottom via repeated write() calls.
 *
 * On boot we run a raw-throughput benchmark against a full-size buffer
 * using three different push strategies. The results tell us whether
 * the current per-strip render is CPU-bound in Galagino, or if the
 * display itself is the ceiling.
 */

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "video.h"
#include "config.h"
#include "hardware/clocks.h"
#include "hardware/spi.h"

static TFT_eSPI tft;

static const int GAME_W = 224;
static const int GAME_H = 288;

/* Landscape mode — set on the build command line via -DPICO_GALAGINO_LANDSCAPE
 * (or in config.h). Rotates the panel to 320x240 and downscales the 288-row
 * game framebuffer to 240 rows with a 6:5 fixed-point bilinear filter so the
 * game fills the shorter dimension of the landscape display. Extra 48px on
 * each horizontal side stays black. */
#ifdef PICO_GALAGINO_LANDSCAPE
static const int LANDSCAPE_H = 240;
#ifdef PICO_GALAGINO_LANDSCAPE_HSCALE
static const int LANDSCAPE_W = 186;
#else
static const int LANDSCAPE_W = GAME_W;
#endif
static uint16_t scaled_buf[LANDSCAPE_W * LANDSCAPE_H];

/* Runtime landscape flag, defined in pico_config.cpp, loaded from SD
 * before Video::begin() runs. Video::begin() reads it ONCE and caches
 * into is_landscape_locked so the per-frame push has no volatile read
 * and no runtime tft.width()/height() calls — matches the hot-path
 * cost of the earlier compile-time-only landscape build. */
extern "C" volatile int g_landscape_mode;
static int is_landscape_locked = 0;
static int push_x_off = 0;
#endif

static int cur_row = 0;

/* Single buffer (double-buffer variant kept as commented-out reference).
 * At 200 MHz sys / 100 MHz SPI, blocking pushImage may be fast enough to
 * still hit real speed with half the RAM footprint. */
static uint16_t frame_buf[GAME_W * GAME_H];

#ifdef PICO_GALAGINO_LANDSCAPE
/* 6:5 vertical downscale — 6 source rows → 5 destination rows.
 *
 * Two modes selected by PICO_GALAGINO_LANDSCAPE_NEAREST:
 *   - Bilinear (default): fixed-point 8-bit weights, blends 2 rows per output.
 *     ~5 ms/frame at 200 MHz — quality: smooth, slightly blurry.
 *   - Nearest-neighbour (define =1): pick 5 of every 6 rows (memcpy only).
 *     ~0.5 ms/frame — quality: sharp but slight jitter on horizontal lines.
 *
 * Framebuffer stores byte-swapped RGB565 (setSwapBytes(false) + palette
 * pre-swap). Bilinear blend bswaps into RGB565 order, blends R+B and G
 * separately using non-overlapping bit fields, then bswaps back. */

#pragma GCC push_options
#pragma GCC optimize("O3")

static const uint16_t VERT_W0[5] = { 256, 205, 154, 102,  51 };
static const uint16_t VERT_W1[5] = {   0,  51, 102, 154, 205 };

#ifdef PICO_GALAGINO_LANDSCAPE_NEAREST
/* Row copy — with or without horizontal 6:5 drop. */
#ifdef PICO_GALAGINO_LANDSCAPE_HSCALE
/* Drop-1-of-6 horizontally: for a group of 6 src pixels write 5 dst,
 * skipping src[3]. 224 → 186 output columns (37 full groups + 1 tail). */
static inline void copy_row565(const uint16_t *src, uint16_t *dst, int /*width*/) {
    for (int g = 0; g < 37; g++) {
        const uint16_t *s = src + g * 6;
        uint16_t *d = dst + g * 5;
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = s[4];   /* skip s[3] */
        d[4] = s[5];
    }
    dst[185] = src[222];   /* tail: 1 extra pixel from the 2 leftover src cols */
}
#else
static inline void copy_row565(const uint16_t *src, uint16_t *dst, int width) {
    memcpy(dst, src, (size_t)width * 2);
}
#endif
#else
static inline void blend_rows565(const uint16_t *s0, const uint16_t *s1,
                                 uint16_t *dst, int width,
                                 uint16_t w0, uint16_t w1) {
    if (w0 == 256) {
        memcpy(dst, s0, (size_t)width * 2);
        return;
    }
    for (int x = 0; x < width; x++) {
        uint32_t p0 = (uint32_t)__builtin_bswap16(s0[x]);
        uint32_t p1 = (uint32_t)__builtin_bswap16(s1[x]);
        uint32_t rb0 = p0 & 0xF81F;
        uint32_t rb1 = p1 & 0xF81F;
        uint32_t g0  = p0 & 0x07E0;
        uint32_t g1  = p1 & 0x07E0;
        uint32_t rb  = ((rb0 * w0 + rb1 * w1) >> 8) & 0xF81F;
        uint32_t g   = ((g0  * w0 + g1  * w1) >> 8) & 0x07E0;
        dst[x] = __builtin_bswap16((uint16_t)(rb | g));
    }
}
#endif

static void downscale_288_to_240(const uint16_t *src, uint16_t *dst) {
    /* 48 blocks × 6 src rows → 5 dst rows = 288 → 240 exactly. */
    for (int j = 0; j < LANDSCAPE_H; j += 5) {
        int base = (j / 5) * 6;
        const uint16_t *r0 = src + (size_t)(base + 0) * GAME_W;
        const uint16_t *r1 = src + (size_t)(base + 1) * GAME_W;
        const uint16_t *r2 = src + (size_t)(base + 2) * GAME_W;
        const uint16_t *r3 = src + (size_t)(base + 3) * GAME_W;
        const uint16_t *r4 = src + (size_t)(base + 4) * GAME_W;
#ifdef PICO_GALAGINO_LANDSCAPE_NEAREST
        /* Nearest-neighbour: for a 6:5 mapping the closest sources for
         * dst rows 0..4 in a block are src rows 0, 1, 2, 4, 5 (skip 3). */
        const uint16_t *r5 = src + (size_t)(base + 5) * GAME_W;
        copy_row565(r0, dst + (size_t)(j + 0) * LANDSCAPE_W, GAME_W);
        copy_row565(r1, dst + (size_t)(j + 1) * LANDSCAPE_W, GAME_W);
        copy_row565(r2, dst + (size_t)(j + 2) * LANDSCAPE_W, GAME_W);
        copy_row565(r4, dst + (size_t)(j + 3) * LANDSCAPE_W, GAME_W);
        copy_row565(r5, dst + (size_t)(j + 4) * LANDSCAPE_W, GAME_W);
#else
        const uint16_t *r5 = src + (size_t)(base + 5) * GAME_W;
        blend_rows565(r0, r1, dst + (size_t)(j + 0) * GAME_W, GAME_W, VERT_W0[0], VERT_W1[0]);
        blend_rows565(r1, r2, dst + (size_t)(j + 1) * GAME_W, GAME_W, VERT_W0[1], VERT_W1[1]);
        blend_rows565(r2, r3, dst + (size_t)(j + 2) * GAME_W, GAME_W, VERT_W0[2], VERT_W1[2]);
        blend_rows565(r3, r4, dst + (size_t)(j + 3) * GAME_W, GAME_W, VERT_W0[3], VERT_W1[3]);
        blend_rows565(r4, r5, dst + (size_t)(j + 4) * GAME_W, GAME_W, VERT_W0[4], VERT_W1[4]);
#endif
    }
}

#pragma GCC pop_options
#endif  /* PICO_GALAGINO_LANDSCAPE */

Video::Video() {}

void Video::begin(void) {
  /* Re-tie clk_peri to clk_sys. When platformio's board_build.f_cpu
   * overrides sys clock, the framework leaves clk_peri on the USB PLL
   * (~48 MHz) which caps SPI to ~24 MHz. Explicitly point clk_peri back
   * at clk_sys so SPI dividers see the full 200 MHz. */
  {
    uint32_t sys_hz = clock_get_hz(clk_sys);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    sys_hz,
                    sys_hz);
  }

  Serial.println("[video] tft.init()");
  tft.init();
#ifdef PICO_GALAGINO_LANDSCAPE
  /* Latch the mode + centering offset ONCE at boot — Video::write can
   * then hot-path both branches with no volatile reads and no per-frame
   * TFT_eSPI api calls. */
  is_landscape_locked = g_landscape_mode ? 1 : 0;
  if (is_landscape_locked) {
    tft.setRotation(3);   /* landscape 320x240, 270° CW */
    push_x_off = (320 - LANDSCAPE_W) / 2;
    Serial.printf("[video] landscape mode ON (image %dx%d at x=%d)\n",
                  LANDSCAPE_W, LANDSCAPE_H, push_x_off);
  } else {
    tft.setRotation(0);   /* portrait 240x320 */
    Serial.println("[video] portrait mode");
  }
#else
  tft.setRotation(0);
#endif
  tft.setSwapBytes(false);
  tft.fillScreen(TFT_BLACK);


  if (!tft.initDMA()) {
    Serial.println("[video] initDMA FAILED — falling back to blocking pushImage");
  } else {
    Serial.println("[video] DMA ready — double-buffered async push enabled");
  }

  cur_row = 0;
  Serial.printf("[video] ready %dx%d game area at (%d,%d)\n",
                GAME_W, GAME_H, TFT_X_OFFSET, TFT_Y_OFFSET);
}

/* The finished frame is sent in bands with the audio topped up between
 * them. Sent in one go, a portrait frame blocks core 0 for ~10 ms at
 * 100 MHz SPI but ~21 ms at 50 MHz (ILI9341 build) — as long as the audio
 * queued ahead, so the PWM output ran dry every few frames. PWMAudio then
 * plays an unscaled "silence" level (full duty), so each underrun was a
 * click, even muted. */
extern void snd_transmit();
static const int PUSH_BANDS = 6;

static void push_banded(int x, int y, int w, int h, uint16_t *buf) {
  int band = (h + PUSH_BANDS - 1) / PUSH_BANDS;
  for (int y0 = 0; y0 < h; y0 += band) {
    int n = (h - y0 < band) ? h - y0 : band;
    tft.pushImage(x, y + y0, w, n, buf + (size_t)y0 * w);
    snd_transmit();
  }
}

void Video::write(uint16_t *colors, uint32_t len) {
  int rows = (int)(len / GAME_W);
  if (rows <= 0) return;

  if (cur_row + rows > GAME_H) rows = GAME_H - cur_row;
  memcpy(&frame_buf[cur_row * GAME_W], colors, (size_t)rows * GAME_W * 2);

  cur_row += rows;
  if (cur_row >= GAME_H) {
#ifdef PICO_GALAGINO_LANDSCAPE
    if (is_landscape_locked) {
      downscale_288_to_240(frame_buf, scaled_buf);
      push_banded(push_x_off, 0, LANDSCAPE_W, LANDSCAPE_H, scaled_buf);
    } else {
      push_banded(TFT_X_OFFSET, TFT_Y_OFFSET, GAME_W, GAME_H, frame_buf);
    }
#else
    push_banded(TFT_X_OFFSET, TFT_Y_OFFSET, GAME_W, GAME_H, frame_buf);
#endif
    cur_row = 0;
    fps_stats_tick();
  }
}

/* ---- FPS / CPU stats ------------------------------------------------------
 * Once a second: screen FPS (frames pushed here), emulation FPS and the
 * average CPU-emulation time per frame (published by emulation.c on core 1),
 * printed to serial and — when enabled in the menu (g_show_fps) — drawn in
 * the black border beside/below the game image so it never covers it. */
extern "C" volatile int g_show_fps;
extern "C" volatile unsigned long g_emu_frames;
extern "C" volatile unsigned long g_emu_us_sum;
extern "C" volatile unsigned g_hs_saves;     /* pico_hiscore.cpp diagnostics */
extern "C" volatile char g_hs_state;

static const float FRAME_BUDGET_MS = 1000.0f / 60.0f;

static void draw_fps_overlay(float scr_fps, float emu_fps, float emu_ms) {
  float load = emu_ms / FRAME_BUDGET_MS * 100.0f;
  uint16_t col = (load < 80.0f) ? TFT_GREEN : (load < 95.0f) ? TFT_YELLOW : TFT_RED;
  tft.setTextSize(1);
#ifdef PICO_GALAGINO_LANDSCAPE
  if (is_landscape_locked) {
    /* Left border: push_x_off px wide (67 px with the 186 px image). */
    int x = 2, w = push_x_off - 3;
    tft.fillRect(0, 0, w + 2, 70, TFT_BLACK);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(x, 2);   tft.printf("SCR %4.1f", scr_fps);
    tft.setCursor(x, 12);  tft.printf("EMU %4.1f", emu_fps);
    tft.setTextColor(col, TFT_BLACK);
    tft.setCursor(x, 26);  tft.printf("CPU");
    tft.setCursor(x, 36);  tft.printf("%5.1fms", emu_ms);
    tft.setCursor(x, 46);  tft.printf("%4.0f%%", load);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(x, 60);  tft.printf("HS %c %u", g_hs_state, g_hs_saves);
    return;
  }
#endif
  /* Portrait: 16 px strip below the 288-row image. */
  int y = TFT_Y_OFFSET + GAME_H + 4;
  tft.fillRect(0, y - 2, 240, 12, TFT_BLACK);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(4, y);
  tft.printf("SCR %4.1f EMU %4.1f ", scr_fps, emu_fps);
  tft.setTextColor(col, TFT_BLACK);
  tft.printf("CPU %4.1fms %3.0f%%", emu_ms, load);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.printf(" %c%u", g_hs_state, g_hs_saves);   /* high scores: state + saves */
}

void Video::fps_stats_tick(void) {
  static uint32_t frames = 0, last_ms = 0, last_frames = 0;
  static unsigned long last_emu_frames = 0, last_emu_us = 0;
  static int shown = 0;
  frames++;
  uint32_t now = millis();
  if (last_ms == 0) { last_ms = now; return; }
  uint32_t dt = now - last_ms;
  if (dt < 1000) return;

  unsigned long ef = g_emu_frames, eus = g_emu_us_sum;
  unsigned long d_ef = ef - last_emu_frames, d_eus = eus - last_emu_us;
  float scr_fps = (frames - last_frames) * 1000.0f / dt;
  float emu_fps = d_ef * 1000.0f / dt;
  float emu_ms  = d_ef ? (d_eus / 1000.0f) / d_ef : 0.0f;
  last_ms = now; last_frames = frames;
  last_emu_frames = ef; last_emu_us = eus;

  Serial.printf("[video] screen %.1f fps  emu %.1f fps  cpu %.2f ms/frame (%.0f%%)\n",
                scr_fps, emu_fps, emu_ms, emu_ms / FRAME_BUDGET_MS * 100.0f);
  if (g_show_fps) {
    draw_fps_overlay(scr_fps, emu_fps, emu_ms);
    shown = 1;
  } else if (shown) {
    /* Turned off: clear what we drew. */
#ifdef PICO_GALAGINO_LANDSCAPE
    if (is_landscape_locked) tft.fillRect(0, 0, push_x_off - 1, 70, TFT_BLACK);
    else
#endif
    tft.fillRect(0, TFT_Y_OFFSET + GAME_H + 2, 240, 12, TFT_BLACK);
    shown = 0;
  }
}
