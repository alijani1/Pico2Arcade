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
#include "pico_board.h"   /* PICO_ROT_LANDSCAPE / PICO_ROT_PORTRAIT */
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
/* Quad / Duo 5200: Scale + Width rows, any width 186-320, scaled band by band
 * into two small buffers (push_landscape_wide) instead of a whole frame. */
#if defined(PICO_GALAGINO_LANDSCAPE_AREA) && defined(PICO_GALAGINO_LANDSCAPE_HSCALE)
#  define LANDSCAPE_WIDE 1
#else
static uint16_t scaled_buf[LANDSCAPE_W * LANDSCAPE_H];
#endif

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

#ifdef PICO_TINY_SCREEN
/* 160x80 panel (Duo Ultimate test, PICO_DUO): the whole 224x288 frame is
 * box-filtered — each output pixel is the average of the ~2.8x2.8 source
 * pixels it covers — down to 80x103 in portrait (the 80x160 way up) or 62x80
 * in landscape, centred. Pixels stay byte-swapped RGB565 like frame_buf.
 * ~3 ms per frame on core 0; the push is tiny (16 KB at most). */
static const int TINY_PW = 80, TINY_PH = 103;   /* portrait */
static const int TINY_LW = 62, TINY_LH = 80;    /* landscape */
/* Landscape widths (menu Width row / MENU tap in a game): 62 = the true
 * shape (224x288 into 80 rows); 75 = exactly 3 source columns per output,
 * 1.2x wide; 112 = exactly 2, 1.8x wide. Even divisions keep thin vertical
 * lines steady (124 — 1.8 columns each, 2x wide — was tried and dropped);
 * 160 = the full panel, 2.6x wide, uneven (1.4 columns each). Each width
 * also has a "crop" step: scaled to 96 rows (exactly 3 source rows each) and
 * the middle 80 shown, 8 cut off top and bottom (3 tile rows of score / lives
 * each) — 20% taller. Steps: 62, 62 crop, 75, 75 crop, 112, ..., 160 crop. */
static const int TINY_STEPS = 8;
static const int TINY_WIDTHS[TINY_STEPS]  = { 62, 62, 75, 75, 112, 112, 160, 160 };
static const int TINY_VHEIGHT[TINY_STEPS] = { 80, 96, 80, 96,  80,  96,  80,  96 };   /* scaled height before the crop */
static const int TINY_MAXW = 160;
static uint16_t tiny_buf[TINY_MAXW * TINY_PH];
static int tiny_w, tiny_h, tiny_x, tiny_y;
static int tiny_vh, tiny_vy0;                   /* scaled height, first of its rows shown */
/* Landscape width index into TINY_WIDTHS, resampled straight to that many
 * columns (real detail, not doubled pixels). Set by the menu or a MENU tap in
 * a game (pico_input.cpp); the next frame re-centres, clears the borders and
 * saves it (pico_config.cpp). */
extern "C" { volatile int g_stretch = 0; }
extern "C" void pico_config_save_stretch(int idx);
static int tiny_stretch = 0;                    /* what the geometry was built for */

/* Scale row in the menu (both orientations here), kept in the settings
 * (pico_config.cpp). Each output pixel averages a window of source pixels:
 *   Smooth: all the ~2.8x2.8 (3.6x3.6 landscape) pixels it covers (default)
 *   Sharp:  only the 2x2 at its centre — crisper, still no dropped detail
 *           at the 2-3 px scale of most lines
 *   Drop:   the one pixel at its centre (nearest neighbour) */
#if !(defined(PICO_GALAGINO_LANDSCAPE_AREA) && defined(PICO_GALAGINO_LANDSCAPE_HSCALE))
extern "C" { volatile int g_scale_mode = SCALE_SMOOTH; }
#endif
static uint8_t tiny_cs[TINY_MAXW], tiny_cn[TINY_MAXW]; /* per output column: first source column, count */
static uint16_t tiny_rs[TINY_PH];                    /* per output row: first source row */
static uint8_t tiny_rn[TINY_PH];                     /*   and count */
static int tiny_mode = -1;                           /* mode the windows were built for */

static void tiny_window(int i, int n, int len, int mode, int &s, int &c) {
  const int a = i * len / n, b = (i + 1) * len / n;   /* the pixels it covers */
  if (mode == SCALE_DROP)       { s = (a + b) / 2; c = 1; }
  else if (mode == SCALE_SHARP) { s = (a + b - 1) / 2; c = 2; if (s + 2 > len) s = len - 2; }
  else                          { s = a; c = b - a; }
}

static void tiny_build(int mode) {
  int s, c;
  for (int i = 0; i < tiny_w; i++) { tiny_window(i, tiny_w, GAME_W, mode, s, c); tiny_cs[i] = s; tiny_cn[i] = c; }
  for (int i = 0; i < tiny_h; i++) { tiny_window(tiny_vy0 + i, tiny_vh, GAME_H, mode, s, c); tiny_rs[i] = s; tiny_rn[i] = c; }
  tiny_mode = mode;
}

static void tiny_setup(int w, int h, int panel_w, int panel_h, int vh = 0) {
  tiny_w = w; tiny_h = h;
  tiny_vh = vh > h ? vh : h;                         /* > h: crop to the middle h rows */
  tiny_vy0 = (tiny_vh - h) / 2;
  tiny_x = (panel_w - w) / 2;
  tiny_y = (panel_h - h) / 2;
  tiny_mode = -1;                                    /* windows built on the first frame */
}

static void downscale_tiny(const uint16_t *src, uint16_t *dst) {
  if (g_scale_mode != tiny_mode) tiny_build(g_scale_mode);
  uint16_t sr[TINY_MAXW], sg[TINY_MAXW], sb[TINY_MAXW];
  for (int oy = 0; oy < tiny_h; oy++) {
    const int y0 = tiny_rs[oy], y1 = y0 + tiny_rn[oy];
    memset(sr, 0, sizeof(sr)); memset(sg, 0, sizeof(sg)); memset(sb, 0, sizeof(sb));
    for (int sy = y0; sy < y1; sy++) {
      const uint16_t *row = src + (size_t)sy * GAME_W;
      for (int ox = 0; ox < tiny_w; ox++) {
        unsigned r = 0, g = 0, b = 0;
        for (int sx = tiny_cs[ox]; sx < tiny_cs[ox] + tiny_cn[ox]; sx++) {
          uint16_t v = __builtin_bswap16(row[sx]);
          r += v >> 11; g += (v >> 5) & 0x3F; b += v & 0x1F;
        }
        sr[ox] += r; sg[ox] += g; sb[ox] += b;
      }
    }
    uint16_t *out = dst + (size_t)oy * tiny_w;
    for (int ox = 0; ox < tiny_w; ox++) {
      const unsigned n = (unsigned)(y1 - y0) * tiny_cn[ox];
      const unsigned r = (sr[ox] + n / 2) / n, g = (sg[ox] + n / 2) / n, b = (sb[ox] + n / 2) / n;
      out[ox] = __builtin_bswap16((uint16_t)((r << 11) | (g << 5) | b));
    }
  }
}
#endif

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

#ifdef LANDSCAPE_WIDE
static const uint32_t SPREAD_M   = 0x07E0F81Fu;
static const uint32_t SPREAD_RND = 8u | (8u << 11) | (8u << 21);   /* +0.5 per field */

static inline uint32_t spread565(uint16_t p) {     /* byte-swapped 565 -> spread */
    uint32_t v = __builtin_bswap16(p);
    return (v | (v << 16)) & SPREAD_M;
}
static inline uint16_t pack565(uint32_t s) {       /* spread -> byte-swapped 565 */
    s &= SPREAD_M;
    return __builtin_bswap16((uint16_t)(s | (s >> 16)));
}
static inline uint32_t mix16(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb) {
    return (a * wa + b * wb + SPREAD_RND) >> 4;
}

#endif

#ifdef LANDSCAPE_WIDE
/* Landscape scale, picked in the menu (Scale row), default Smooth, and the
 * landscape Width (Width row / MENU tap in a game). Both ride in the handoff
 * flags (pico_config.cpp). */
extern "C" { volatile int g_scale_mode = SCALE_SMOOTH; volatile int g_stretch = 0; }
extern "C" void pico_config_save_stretch(int idx);

/* Width steps: 186 = the true shape (6:5 both ways); 224 = one screen column
 * per game column (1.2x wide); 268 (1.44x); 320 = the full panel (1.72x).
 * Each also has a "crop" step: rows 1:1 instead of 6:5, the middle 240 of the
 * 288 shown (24 cut off top and bottom, 3 tile rows of score / lives each),
 * 20% taller. 224 crop is the game's own pixels, unscaled. */
static const int WIDE_STEPS = 8;
static const int WIDE_WIDTHS[WIDE_STEPS] = { 186, 186, 224, 224, 268, 268, 320, 320 };
static const int WIDE_MAXW = 320;
static const int WIDE_BAND = LANDSCAPE_H / 6;                /* 40 rows per DMA band */

/* Separable 2-tap filter: output i = w0 x source[a] + w1 x source[a+1], in
 * 16ths. Output i covers source [i*num/den, (i+1)*num/den) (num/den = source
 * pixels per output, <= 1.2, so at most two source pixels):
 *   Smooth: weights = how much of each it covers (area average; for 6:5 the
 *           5+1, 4+2, 3+3, 2+4, 1+5 sixths as 13+3, 11+5, 8+8, 5+11, 3+13)
 *   Sharp:  a minor weight of a third or less snaps to the major pixel, so
 *           most lines are exact copies and only true in-betweens blend
 *   Drop:   the major pixel only (nearest; for 6:5 = skip every 6th)
 * Pixels are "spread" into one 32-bit word, G in the top half and R|B in the
 * bottom (0x07E0F81F), so one multiply weights all three channels: with
 * weights summing to 16 no field reaches the next, and >> 4 divides them all. */
struct Tap { uint16_t a; uint8_t w0, w1; };
static Tap wide_cols[WIDE_MAXW], wide_rows[LANDSCAPE_H];
static int wide_w = 186, wide_built_mode = -1, wide_built_step = -1;
static bool wide_cols_identity = false;                     /* 224: column i = source i */
static uint16_t wide_band[2][WIDE_BAND * WIDE_MAXW];         /* DMA double buffer */

static void build_taps(Tap *t, int n, int num, int den, int ofs, int src_len, int mode) {
  for (int i = 0; i < n; i++) {
    const int start = i * num, end = start + num;            /* in 1/den source pixels */
    const int a = start / den;
    int cov = (a + 1) * den; if (cov > end) cov = end; cov -= start;
    int w0 = (16 * cov + num / 2) / num, w1 = 16 - w0;
    if (mode == SCALE_DROP)       { if (w1 > w0) { w0 = 16; w1 = 0; t[i].a = ofs + a + 1; } else { w0 = 16; w1 = 0; t[i].a = ofs + a; } }
    else if (mode == SCALE_SHARP && (w0 <= 5 || w1 <= 5)) {
      t[i].a = ofs + (w1 > w0 ? a + 1 : a); w0 = 16; w1 = 0;
    } else t[i].a = ofs + a;
    if (w1 && t[i].a + 1 >= src_len) { w0 = 16; w1 = 0; }    /* never past the last pixel */
    t[i].w0 = (uint8_t)w0; t[i].w1 = (uint8_t)w1;
  }
}

static void wide_build(int mode, int step) {
  wide_w = WIDE_WIDTHS[step];
  if (wide_w == 186) build_taps(wide_cols, wide_w, 6, 5, 0, GAME_W, mode);   /* exact 6:5 groups */
  else               build_taps(wide_cols, wide_w, GAME_W, wide_w, 0, GAME_W, mode);
  if (step & 1) build_taps(wide_rows, LANDSCAPE_H, 1, 1, (GAME_H - LANDSCAPE_H) / 2, GAME_H, mode);  /* crop */
  else          build_taps(wide_rows, LANDSCAPE_H, 6, 5, 0, GAME_H, mode);
  wide_cols_identity = (wide_w == GAME_W);
  wide_built_mode = mode; wide_built_step = step;
}

/* Source rows spread once each: a two-row cache (rows come in rising order). */
static uint32_t wide_sp[2][GAME_W];
static int wide_sp_row[2] = { -1, -1 };
static const uint32_t *spread_row(const uint16_t *src, int r, int keep) {
  if (wide_sp_row[0] == r) return wide_sp[0];
  if (wide_sp_row[1] == r) return wide_sp[1];
  int k = (wide_sp_row[0] == keep) ? 1 : (wide_sp_row[1] == keep) ? 0
        : (wide_sp_row[0] < wide_sp_row[1] ? 0 : 1);
  const uint16_t *s = src + (size_t)r * GAME_W;
  for (int x = 0; x < GAME_W; x++) wide_sp[k][x] = spread565(s[x]);
  wide_sp_row[k] = r;
  return wide_sp[k];
}

static void wide_scale_rows(const uint16_t *src, uint16_t *dst, int oy0, int oy1) {
  static uint32_t vrow[GAME_W];
  for (int oy = oy0; oy < oy1; oy++) {
    const Tap ty = wide_rows[oy];
    uint16_t *out = dst + (size_t)(oy - oy0) * wide_w;
    if (!ty.w1 && wide_cols_identity) {                       /* a pure row at 224: copy */
      memcpy(out, src + (size_t)ty.a * GAME_W, GAME_W * 2);
      continue;
    }
    const uint32_t *v = spread_row(src, ty.a, -1);
    if (ty.w1) {
      const uint32_t *v1 = spread_row(src, ty.a + 1, ty.a);
      v = spread_row(src, ty.a, ty.a + 1);
      for (int x = 0; x < GAME_W; x++) vrow[x] = mix16(v[x], v1[x], ty.w0, ty.w1) & SPREAD_M;
      v = vrow;
    }
    for (int ox = 0; ox < wide_w; ox++) {
      const Tap tx = wide_cols[ox];
      out[ox] = tx.w1 ? pack565(mix16(v[tx.a], v[tx.a + 1], tx.w0, tx.w1)) : pack565(v[tx.a]);
    }
  }
}
#endif  /* LANDSCAPE_WIDE */


#ifndef LANDSCAPE_WIDE
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
#endif  /* !LANDSCAPE_WIDE */

#pragma GCC pop_options
#endif  /* PICO_GALAGINO_LANDSCAPE */

Video::Video() {}

#ifdef PICO_TINY_SCREEN
/* The menu and ROM drive screens draw through this same object: on the ST7735
 * the panel offsets (colstart/rowstart) depend on the tab type that only
 * init() records, so a second, un-init'd TFT_eSPI drew a few pixels off. */
TFT_eSPI &video_tft(void) { return tft; }
#endif

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
    tft.setRotation(PICO_ROT_LANDSCAPE);   /* landscape 320x240, 270° CW */
    push_x_off = (320 - LANDSCAPE_W) / 2;
#ifdef LANDSCAPE_WIDE
    if (g_stretch < 0 || g_stretch >= WIDE_STEPS) g_stretch = 0;
    push_x_off = (320 - WIDE_WIDTHS[g_stretch]) / 2;
#endif
    Serial.printf("[video] landscape mode ON (image %dx%d at x=%d)\n",
                  LANDSCAPE_W, LANDSCAPE_H, push_x_off);
  } else {
    tft.setRotation(PICO_ROT_PORTRAIT);   /* portrait 240x320 */
    Serial.println("[video] portrait mode");
  }
#ifdef PICO_TINY_SCREEN
  tiny_stretch = g_stretch;
  if (is_landscape_locked) tiny_setup(TINY_WIDTHS[tiny_stretch], TINY_LH, tft.width(), tft.height(),
                                      TINY_VHEIGHT[tiny_stretch]);
  else                     tiny_setup(TINY_PW, TINY_PH, tft.width(), tft.height());
  push_x_off = tiny_x;
  Serial.printf("[video] tiny screen: image %dx%d at (%d,%d)\n", tiny_w, tiny_h, tiny_x, tiny_y);
#endif
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

#ifdef LANDSCAPE_WIDE
/* Landscape: scale a band (40 rows) into one of two band buffers, start its
 * DMA push, and scale the next band into the other while it goes out — the
 * push no longer adds to the scaling time (pushImageDMA waits for the band
 * before, so a buffer is free again two bands later). Audio topped up between
 * bands as before. Without DMA: the same bands, pushed blocking. A changed
 * Scale or Width rebuilds the taps; a changed width re-centres, clears the
 * old borders and is saved. */
static void push_landscape_wide(void) {
  const int mode = g_scale_mode, step = g_stretch;
  if (mode != wide_built_mode || step != wide_built_step) {
    const int oldw = wide_w;
    wide_build(mode, step);
    wide_sp_row[0] = wide_sp_row[1] = -1;
    if (wide_w != oldw || push_x_off != (320 - wide_w) / 2) {
      push_x_off = (320 - wide_w) / 2;
      tft.fillScreen(TFT_BLACK);
    }
    pico_config_save_stretch(step);           /* no-op if unchanged */
  }
  wide_sp_row[0] = wide_sp_row[1] = -1;       /* a new frame */
  const bool dma = tft.DMA_Enabled;
  if (dma) tft.startWrite();
  for (int b = 0, y0 = 0; y0 < LANDSCAPE_H; b ^= 1, y0 += WIDE_BAND) {
    wide_scale_rows(frame_buf, wide_band[b], y0, y0 + WIDE_BAND);
    if (dma) tft.pushImageDMA(push_x_off, y0, wide_w, WIDE_BAND, wide_band[b]);   /* waits for the band before */
    else     tft.pushImage(push_x_off, y0, wide_w, WIDE_BAND, wide_band[b]);
    snd_transmit();
  }
  if (dma) { tft.dmaWait(); tft.endWrite(); }
}
#endif

void Video::write(uint16_t *colors, uint32_t len) {
  int rows = (int)(len / GAME_W);
  if (rows <= 0) return;

  if (cur_row + rows > GAME_H) rows = GAME_H - cur_row;
  memcpy(&frame_buf[cur_row * GAME_W], colors, (size_t)rows * GAME_W * 2);

  cur_row += rows;
  if (cur_row >= GAME_H) {
#if defined(PICO_TINY_SCREEN)
    if (is_landscape_locked && g_stretch != tiny_stretch) {   /* Width changed (menu / MENU tap) */
      tiny_stretch = g_stretch;
      tiny_setup(TINY_WIDTHS[tiny_stretch], TINY_LH, tft.width(), tft.height(), TINY_VHEIGHT[tiny_stretch]);
      push_x_off = tiny_x;
      tft.fillScreen(TFT_BLACK);
      pico_config_save_stretch(tiny_stretch);     /* no-op if the menu already saved it */
    }
    downscale_tiny(frame_buf, tiny_buf);
    push_banded(tiny_x, tiny_y, tiny_w, tiny_h, tiny_buf);
#elif defined(PICO_GALAGINO_LANDSCAPE)
    if (is_landscape_locked) {
#ifdef LANDSCAPE_WIDE
      push_landscape_wide();
#else
      downscale_288_to_240(frame_buf, scaled_buf);
      push_banded(push_x_off, 0, LANDSCAPE_W, LANDSCAPE_H, scaled_buf);
#endif
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
#ifdef PICO_TINY_SCREEN
  if (!is_landscape_locked) {
    /* Portrait 80x160: two short lines in the strip above the image. */
    tft.fillRect(0, 0, tiny_w, tiny_y - 1, TFT_BLACK);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(0, 2);   tft.printf("S%2.0f E%2.0f", scr_fps, emu_fps);
    tft.setTextColor(col, TFT_BLACK);
    tft.setCursor(0, 12);  tft.printf("CPU %3.0f%%", load);
    return;
  }
  if (tiny_x < 18) return;      /* full width: no border to draw in (serial only) */
  {
    /* Landscape: a 24-49 px border — three numbers, 3 chars each. */
    tft.fillRect(0, 0, tiny_x, 36, TFT_BLACK);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(0, 2);   tft.printf("%2.0f", scr_fps);
    tft.setCursor(0, 12);  tft.printf("%2.0f", emu_fps);
    tft.setTextColor(col, TFT_BLACK);
    tft.setCursor(0, 22);  tft.printf("%3.0f", load);
    return;
  }
#endif
#ifdef PICO_GALAGINO_LANDSCAPE
  if (is_landscape_locked) {
    /* Left border: push_x_off px wide (67 px with the 186 px image). */
    int x = 2, w = push_x_off - 3;
#ifdef LANDSCAPE_WIDE
    if (push_x_off < 20) return;             /* 320: no border (serial only) */
    if (push_x_off < 60) {                   /* 224 / 268: three numbers, 3 chars each */
      tft.fillRect(0, 0, push_x_off, 36, TFT_BLACK);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.setCursor(0, 2);   tft.printf("%2.0f", scr_fps);
      tft.setCursor(0, 12);  tft.printf("%2.0f", emu_fps);
      tft.setTextColor(col, TFT_BLACK);
      tft.setCursor(0, 22);  tft.printf("%3.0f", load);
      return;
    }
#endif
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
    if (is_landscape_locked) { if (push_x_off > 1) tft.fillRect(0, 0, push_x_off - 1, 70, TFT_BLACK); }
    else
#endif
#ifdef PICO_TINY_SCREEN
    tft.fillRect(0, 0, tiny_w, tiny_y - 1, TFT_BLACK);
    if (0)
#endif
    tft.fillRect(0, TFT_Y_OFFSET + GAME_H + 2, 240, 12, TFT_BLACK);
    shown = 0;
  }
}
