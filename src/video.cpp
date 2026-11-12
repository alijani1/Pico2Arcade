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
/* (HDMI: the menu canvas's 77 KB are the start of this buffer too — HdmiCanvas
 * below. As a canvas of its own on the heap, next to this idle buffer,
 * Xevious (~200 KB of heap at its peak while loading) failed to load on the
 * Fruit Jam; made on the heap at game start instead, the freed zip and canvas
 * left no 129 KB in one piece after the load.) */
#ifdef PICO_HDMI
/* (word-aligned: the line interrupt's 5:3 path reads its rows a word at a time) */
static uint16_t __attribute__((aligned(4))) frame_buf[GAME_W * GAME_H];
#else
static uint16_t frame_buf[GAME_W * GAME_H];
#endif

#ifdef PICO_HDMI
/* ---- HDMI output (Adafruit Fruit Jam) -------------------------------------
 * 800x480 at 60 Hz (by default; see HDMI_MODES) through Pimoroni's dvhstx
 * driver (from Adafruit's Adafruit-DVI-HSTX library; only its driver is
 * built, at the end of this file) in scanline mode: there is no frame buffer
 * for the output. The driver's DMA interrupt asks for each output line just
 * before it goes out, and hdmi_line() makes it, two RGB565 pixels a word. It
 * runs on core 0 (the renderer's; at first core 1, the emulator's — see
 * hdmi_start) at the highest priority, from RAM, and reads only RAM.
 * The layout follows Pico2Atari8's src/a8_display.cpp (HDMI half) and its
 * docs/FRUITJAM_PORTING.md.
 *
 * In a game: the lines come straight from frame_buf (Galagino's 224x288 frame,
 * byte-swapped RGB565, filled strip by strip by Video::write on core 0),
 * scaled to the mode's full height with square pixels: 480 lines = 5:3 both
 * ways (373 x 480 in the middle of 800, black at the sides), 576 = exactly
 * 2x. Nearest neighbour through a column map and a row map built when the
 * output starts. An output line whose source row is the one before's (a
 * scale above 1) is copied from a one-line cache instead of being made again.
 *
 * Menus (game list, load errors, ROM drive mode): the existing TFT_eSPI
 * drawing code draws into an 8-bit (RGB332) sprite, the 320x240 canvas,
 * shown doubled (640x480) through a palette of doubled RGB565 words. Its
 * pixels are the first 77 KB of frame_buf (HdmiCanvas::attach), which the
 * game takes over when it starts.
 *
 * Clock: the driver's start-up moves clk_sys to PLL_USB / 2 = 240 MHz and
 * runs HSTX from PLL_SYS at the mode's exact clock; platformio.ini leaves
 * f_cpu at 150 MHz so the core's main() doesn't set clk_sys back.
 *
 * TODO: tearing — Video::write fills frame_buf (30 Hz, half-rate pacing in
 * galagino.cpp update_screen) while the lines are read (60 Hz); if a tear line
 * shows, fill it at the HDMI vblank.
 * (Done: it showed in Xevious - a wavy, flickering scrolling background. On
 * HDMI update_screen now renders every frame whole from the vertical blank,
 * video_hdmi_wait_vblank.)
 * TODO: flash writes (high scores, settings, ROM drive) park core 1
 * (rp2040.idleOtherCore), and with it maybe the line interrupt: the picture
 * may drop out for that moment (the driver restarts its stream after).
 * TODO: the FPS overlay (Show FPS) is on the serial log only here.
 * TODO: a Screen mode setting (tried for 10 s before it is kept, button held
 * at power-on back to the default) as in Pico2Atari8's a8_settings.cpp. */
#include "drivers/dvhstx/dvhstx.hpp"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/gpio.h"

static pimoroni::DVHSTX hdmi;

/* The menus' canvas. fillScreen() fills TFT_eSPI's _width x _height, which a
 * sprite leaves at the panel size of the build flags (240 x 320 — init() never
 * runs on it), so it takes its real size (as Pico2Atari8's A8Canvas). */
class HdmiCanvas : public TFT_eSprite {
public:
  explicit HdmiCanvas(TFT_eSPI *parent) : TFT_eSprite(parent) {}
  void set_screen_size(int32_t w, int32_t h) {
    _init_width = _width = w;
    _init_height = _height = h;
  }
  /* An 8-bit sprite on memory we own (the start of frame_buf), set up as
   * TFT_eSprite::createSprite does — it has no way to take a buffer, and its
   * fields are protected. Never deleteSprite() it (that would free the
   * buffer): detach() instead. */
  uint8_t *attach(uint8_t *buf, int16_t w, int16_t h) {
    setColorDepth(8);
    _iwidth = _dwidth = _bitwidth = w;
    _iheight = _dheight = h;
    cursor_x = cursor_y = 0;
    _sx = _sy = 0;
    _sw = w;
    _sh = h;
    _scolor = TFT_BLACK;
    _img8 = _img8_1 = _img8_2 = buf;
    _img = (uint16_t *)buf;
    _img4 = buf;
    _created = true;
    rotation = 0;
    setViewport(0, 0, _dwidth, _dheight);
    setPivot(_iwidth / 2, _iheight / 2);
    return buf;
  }
  void detach(void) {
    _created = false;
    _img8 = _img8_1 = _img8_2 = nullptr;
    _img = nullptr;
    _img4 = nullptr;
    _vpOoB = true;
  }
};
static HdmiCanvas canvas(&tft);
TFT_eSPI &video_tft(void) { return canvas; }

static const int CANVAS_W = 320, CANVAS_H = 240;
static uint8_t * volatile canvas8 = nullptr;           /* the canvas's pixels (RGB332) */
static uint32_t hdmi_pal[256];                          /* RGB332 -> an RGB565 pixel twice */

/* The video mode: w x h the driver's frame size; scanline mode doubles it
 * (the callback draws whole output lines). Displays differ in what they take
 * (Pico2Atari8's first one: nothing at 640x480, but 720x400, 720x576 and
 * 800x480; 800x600 only sometimes). The 800x600 timing is fixed at the end of
 * this file. */
/* (Since: the game list's Screen row picks one, tried for 10 s before it is
 * kept (pico_menu.cpp), saved; button 3 held at power-on = the default -
 * as Pico2Atari8's Settings > Screen mode. hz: the game still runs at 60 Hz
 * in the 50 / 70 Hz modes, video_hdmi_emu_frames.) */
struct HdmiMode { uint16_t w, h; const char *name; uint8_t hz; };
static const HdmiMode HDMI_MODES[] = {
  { 320, 240, "640x480 60 Hz", 60 },
  { 360, 200, "720x400 70 Hz", 70 },
  { 360, 240, "720x480 60 Hz", 60 },
  { 360, 288, "720x576 50 Hz", 50 },
  { 400, 300, "800x600 60 Hz", 60 },
  { 400, 240, "800x480 60 Hz", 60 },
};
static const int HDMI_NMODES = sizeof(HDMI_MODES) / sizeof(HDMI_MODES[0]);
#ifndef PICO_HDMI_MODE
#define PICO_HDMI_MODE 5                                /* index into HDMI_MODES: 800x480 @ 60 Hz */
#endif
/* The mode to start in: pico_config.cpp's (saved; -1 = the default). */
extern "C" { volatile int g_hdmi_mode = -1; }
extern "C" void pico_config_save_hdmi_mode(int m);      /* pico_config.cpp (-1: the default) */
extern "C" int video_hdmi_mode_default(void);
static int hdmi_vmode = PICO_HDMI_MODE;                 /* the mode running */

static volatile bool hdmi_wanted = false, hdmi_started = false, hdmi_ok = false;
static volatile bool hdmi_game = false;                 /* lines from frame_buf, else the canvas */
static int out_words = 400, out_lines = 480;            /* the mode's line: words (2 pixels), lines */

/* The game picture: pic_words words from word pic_pad of each line. */
static const int HDMI_MAX_PX = 800, HDMI_MAX_LINES = 600;
static const uint16_t NO_ROW = 0xFFFF;
static const uint16_t ROW_BLEND = 0x8000;               /* row_map: the mean of this row and the next */
static const uint16_t COL_BLEND = 0x100;                /* col_map: the mean of this column and the next */
static int pic_words = 0, pic_pad = 0;
static uint16_t col_map[HDMI_MAX_PX];                   /* output pixel -> source column (| COL_BLEND) */
static uint16_t row_map[HDMI_MAX_LINES];                /* output line -> source row (| ROW_BLEND), NO_ROW = black */

/* Scale and Width (the game list's rows, as the Quad / Duo 5200 / Duo; kept
 * in the settings, pico_config.cpp; serial keys z / v in a game):
 *   Scale  Drop   - each output pixel / line from the source one nearest
 *                   its centre (at 5:3 some rows are 2 lines, some 1: a
 *                   scrolling picture's lines change thickness as they go)
 *          Sharp  - lines that straddle two rows show their mean; columns
 *                   as Drop (crisp across, steady lines when scrolling)
 *          Smooth - the same for columns too (default)
 *   Width  372 (the true shape at 480 lines, 5:3 both ways), 448 (each
 *          column twice, 1.2x wide), 560 (2.5x), 672 (3x) - about 2x the Quad's 186 / 224 /
 *          268 / 320 - each also "crop": the middle 240 of the 288 rows,
 *          each exactly twice (24 cut off top and bottom: 3 tile rows of
 *          score / lives each), 20 % taller. Other HDMI heights: in
 *          proportion (lines / 480).
 * A "mean" is a 50/50 blend (at 5:3 each straddling line / column is 1/3 -
 * 2/3, so it is the nearer). Made in the line interrupt: each source row once
 * per frame, scaled across into one of two row buffers; each line a copy of
 * one, or the mean of two. */
extern "C" { volatile int g_scale_mode = SCALE_SMOOTH; volatile int g_stretch = 2; }   /* (HDMI default: Smooth 448) */
extern "C" void pico_config_save_stretch(int idx);
extern "C" void pico_config_save_scale(int mode);
static const int HDMI_WIDTHS[4] = { 372, 448, 560, 672 };   /* (536 / 640 at first: no direct path, up to 87 % of core 0) */
static int pic_mode = -1, pic_step = -1;                /* what the maps were built for */

#pragma GCC push_options
/* no-tree-loop-distribute-patterns: the fill / copy loops must stay loops — GCC
 * makes them memset / memcpy calls otherwise, which live in flash (the line
 * interrupt must not touch flash). */
#pragma GCC optimize("O2", "no-tree-loop-distribute-patterns")

/* Two frame_buf pixels (byte-swapped RGB565) -> one output word, the first
 * in the low half: REV16 swaps the bytes of both halves at once. */
static inline __attribute__((always_inline)) uint32_t hdmi_px2(uint32_t a, uint32_t b) {
  uint32_t v = a | (b << 16), r;
  __asm__("rev16 %0, %1" : "=r"(r) : "r"(v));
  return r;
}

static inline __attribute__((always_inline)) void hdmi_fill(uint32_t *d, int n, uint32_t v) {
  for (int i = 0; i < n; i++) d[i] = v;
}

/* Diagnosis: the line interrupt's time and calls (the stats line, once a second). */
static volatile uint32_t hdmi_irq_us = 0, hdmi_irq_lines = 0;
/* core 0's render time per frame (galagino.cpp update_screen), for the same line */
volatile uint32_t g_hdmi_render_us = 0, g_hdmi_render_n = 0;

/* One output line (the driver's interrupt, core 1). */
static inline __attribute__((always_inline)) void hdmi_line_body(uint32_t active_line, uint32_t *dst);
static void __not_in_flash_func(hdmi_line)(uint32_t v_scanline, uint32_t active_line, uint32_t *dst) {
  (void)v_scanline;
  const uint32_t t0 = timer_hw->timerawl;
  hdmi_line_body(active_line, dst);
  hdmi_irq_us += timer_hw->timerawl - t0;
  hdmi_irq_lines++;
}

/* The game rows, made once each into line_cache (and copied out for a row
 * shown twice). The line interrupt shares core 1 with the emulator: on the
 * board the first version (column map for every word, black sides written
 * on every line) took 16 us a line, 46 % of core 1, and the emulator ran at
 * 48 fps (2026-11-11). Now:
 *   - the black sides are written once per driver line buffer (it cycles
 *     through a few fixed ones), not on every line (border_buf);
 *   - 480 lines (5:3, the default): each 6 source pixels are exactly 5 output
 *     words — (s0 s0) (s1 s1) (s2 s3) (s3 s4) (s4 s5) — made from 3 word
 *     loads, no column map; 576 lines (2x): each source pixel twice;
 *   - rows shown twice: copied 8 words at a time.
 * Other heights: the column map, as before.
 * (Since: the Scale / Width rows above. The row cache is two rows (a line
 * may be the mean of two); the direct paths are by width: 372 at 480 lines
 * (5:3: Drop (s0 s0) (s1 s2) (s2 s3) (s3 s4) (s5 s5) by the pixel centres,
 * Smooth (s0 m01) (s1 m12) (s2 s3) (m34 s4) (m45 s5)) and 448 (each pixel
 * twice); other widths the column map. And the interrupt is on core 0 now.) */
static int pic_fast = 0;   /* 1: 372 Drop / Sharp, 3: 372 Smooth, 2: 448, 4 / 5: 560 (Smooth 5), 6: 672, 0: col_map */
static uint32_t *border_buf[4];                         /* line buffers whose sides are black (game) */
static int border_n = 0;
static bool border_game = false;                        /* border_buf is for the game picture */
/* The overlay box (Show FPS: fps and CPU, and the Scale / Width; the Scale /
 * Width alone for 3 s after a change and at game start): OV_W x OV_H pixels
 * drawn by core 0 (hdmi_overlay_draw, TFT_eSprite) into the back one of two
 * buffers, then swapped in; shown 2 x 2 at word OV_X, line OV_Y. Over the
 * picture where it reaches it (the widest widths), else in the side band. */
static const int OV_W = 80, OV_H = 42, OV_X = 4, OV_Y = 8;   /* (13 characters: "720x576 50 Hz") */
static uint16_t ov_buf[2][OV_W * OV_H];                 /* RGB565 (not byte-swapped) */
static const uint16_t * volatile ov_front = ov_buf[0];
static volatile bool ov_show = false, ov_clear = false;
static uint32_t row_buf[2][HDMI_MAX_PX / 2];            /* two source rows, scaled across */
static int row_buf_r[2] = { -1, -1 };

static inline __attribute__((always_inline)) uint32_t hdmi_rev16(uint32_t v) {
  uint32_t r;
  __asm__("rev16 %0, %1" : "=r"(r) : "r"(v));
  return r;
}

/* The mean of two RGB565 pixel pairs (each field's lowest bit dropped first,
 * so nothing carries into the next field or pixel). */
static inline __attribute__((always_inline)) uint32_t hdmi_mean(uint32_t a, uint32_t b) {
  return ((a & 0xF7DEF7DEu) >> 1) + ((b & 0xF7DEF7DEu) >> 1);
}

static inline __attribute__((always_inline)) void hdmi_copy(uint32_t *d, const uint32_t *s, int n) {
  for (; n >= 8; n -= 8, d += 8, s += 8) {
    const uint32_t a = s[0], b = s[1], c = s[2], e = s[3], f = s[4], g = s[5], h = s[6], i = s[7];
    d[0] = a; d[1] = b; d[2] = c; d[3] = e; d[4] = f; d[5] = g; d[6] = h; d[7] = i;
  }
  for (; n > 0; n--) *d++ = *s++;
}

/* Source row r, scaled across, into o (pic_words words). */
static inline __attribute__((always_inline)) void hdmi_make_row(int r, uint32_t *o) {
  const uint16_t *s = frame_buf + r * GAME_W;
  if (pic_fast == 1 || pic_fast == 3) {
    const uint32_t *w = (const uint32_t *)s;            /* 2 pixels a word (rows start word-aligned) */
    const bool smooth = pic_fast == 3;
    for (int g = 0; g < GAME_W / 6; g++, w += 3, o += 5) {
      const uint32_t a = hdmi_rev16(w[0]), b = hdmi_rev16(w[1]), c = hdmi_rev16(w[2]);   /* s0|s1, s2|s3, s4|s5 */
      const uint32_t s0 = a & 0xFFFFu, s1 = a >> 16, s2 = b & 0xFFFFu, s3 = b >> 16, s4 = c & 0xFFFFu, s5 = c >> 16;
      if (smooth) {
        const uint32_t m01 = hdmi_mean(s0, s1), m12 = hdmi_mean(s1, s2);
        const uint32_t m34 = hdmi_mean(s3, s4), m45 = hdmi_mean(s4, s5);
        o[0] = s0 | (m01 << 16);
        o[1] = s1 | (m12 << 16);
        o[2] = b;
        o[3] = m34 | (s4 << 16);
        o[4] = m45 | (s5 << 16);
      } else {
        o[0] = s0 | (s0 << 16);
        o[1] = s1 | (s2 << 16);
        o[2] = b;
        o[3] = s3 | (s4 << 16);
        o[4] = s5 | (s5 << 16);
      }
    }
    *o = hdmi_rev16(w[0]);                              /* the last 2 of 224: s222 s223 */
  } else if (pic_fast == 2) {
    const uint32_t *w = (const uint32_t *)s;            /* 2 pixels a load (rows start word-aligned) */
    for (int x = 0; x < GAME_W; x += 2, w++, o += 2) {
      const uint32_t a = hdmi_rev16(w[0]);              /* s0|s1 */
      o[0] = (a & 0xFFFFu) | (a << 16);
      o[1] = (a >> 16) | (a & 0xFFFF0000u);
    }
  } else if (pic_fast == 4 || pic_fast == 5) {
    /* 560 = 2.5x: each 2 source pixels -> 5: s0 s0 X s1 s1, X = s0 (Drop /
     * Sharp; its centre is on the boundary) or their mean (Smooth) */
    const uint32_t *w = (const uint32_t *)s;
    const bool smooth = pic_fast == 5;
    for (int g = 0; g < GAME_W / 4; g++, w += 2, o += 5) {
      const uint32_t a = hdmi_rev16(w[0]), b = hdmi_rev16(w[1]);   /* s0|s1, s2|s3 */
      const uint32_t s0 = a & 0xFFFFu, s1 = a >> 16, s2 = b & 0xFFFFu, s3 = b >> 16;
      const uint32_t x01 = smooth ? hdmi_mean(s0, s1) : s0, x23 = smooth ? hdmi_mean(s2, s3) : s2;
      o[0] = s0 | (s0 << 16);
      o[1] = x01 | (s1 << 16);
      o[2] = s1 | (s2 << 16);
      o[3] = s2 | (x23 << 16);
      o[4] = s3 | (s3 << 16);
    }
  } else if (pic_fast == 7 || pic_fast == 8) {
    /* 336 = 1.5x: each 4 source pixels -> 6: Drop / Sharp s0 s1 s1 s2 s3 s3
     * (by the pixel centres), Smooth s0 m01 s1 s2 m23 s3 */
    const uint32_t *w = (const uint32_t *)s;
    const bool smooth = pic_fast == 8;
    for (int g = 0; g < GAME_W / 4; g++, w += 2, o += 3) {
      const uint32_t a = hdmi_rev16(w[0]), b = hdmi_rev16(w[1]);   /* s0|s1, s2|s3 */
      const uint32_t s0 = a & 0xFFFFu, s1 = a >> 16, s2 = b & 0xFFFFu, s3 = b >> 16;
      if (smooth) {
        o[0] = s0 | (hdmi_mean(s0, s1) << 16);
        o[1] = s1 | (s2 << 16);
        o[2] = hdmi_mean(s2, s3) | (s3 << 16);
      } else {
        o[0] = a;
        o[1] = s1 | (s2 << 16);
        o[2] = s3 | (s3 << 16);
      }
    }
  } else if (pic_fast == 6) {
    /* 672 = 3x: each source pixel three times */
    const uint32_t *w = (const uint32_t *)s;
    for (int g = 0; g < GAME_W / 2; g++, w++, o += 3) {
      const uint32_t a = hdmi_rev16(w[0]), s0 = a & 0xFFFFu, s1 = a >> 16;
      o[0] = s0 | (s0 << 16);
      o[1] = a;
      o[2] = s1 | (s1 << 16);
    }
  } else {
    const uint16_t *cm = col_map;
    for (int w = 0; w < pic_words; w++, cm += 2) {
      uint32_t px[2];
      for (int k = 0; k < 2; k++) {
        const uint32_t m = cm[k], c = m & 0xFF;
        uint32_t v = hdmi_rev16(s[c]) & 0xFFFFu;
        if (m & COL_BLEND) v = hdmi_mean(v, hdmi_rev16(s[c + 1]) & 0xFFFFu);
        px[k] = v;
      }
      o[w] = px[0] | (px[1] << 16);
    }
  }
}

/* Row r scaled across, from the two-row cache (keep: a row not to evict). */
static inline __attribute__((always_inline)) const uint32_t *hdmi_row(int r, int keep) {
  if (row_buf_r[0] == r) return row_buf[0];
  if (row_buf_r[1] == r) return row_buf[1];
  const int k = (row_buf_r[0] == keep) ? 1 : (row_buf_r[1] == keep) ? 0
              : (row_buf_r[0] < row_buf_r[1] ? 0 : 1);
  hdmi_make_row(r, row_buf[k]);
  row_buf_r[k] = r;
  return row_buf[k];
}

static inline __attribute__((always_inline)) void hdmi_line_body(uint32_t active_line, uint32_t *dst) {
  const int words = out_words;
  if (!hdmi_game) {
    border_game = false;
    /* the canvas, each pixel twice across, each row twice down, centred */
    const uint8_t *c = canvas8;
    const int pad = (words - CANVAS_W) / 2;
    const int r = ((int)active_line - (out_lines - 2 * CANVAS_H) / 2) >> 1;
    if (!c || r < 0 || r >= CANVAS_H || pad < 0) { hdmi_fill(dst, words, 0); return; }
    hdmi_fill(dst, pad, 0);
    hdmi_fill(dst + pad + CANVAS_W, words - pad - CANVAS_W, 0);
    const uint8_t *s = c + r * CANVAS_W;
    uint32_t *d = dst + pad;
    for (int x = 0; x < CANVAS_W; x++) d[x] = hdmi_pal[s[x]];
    return;
  }
  if (!border_game) { border_game = true; border_n = 0; }   /* the canvas used other sides */
  if (ov_clear) { ov_clear = false; border_n = 0; }         /* the box went: sides black again */
  if (active_line == 0) row_buf_r[0] = row_buf_r[1] = -1;   /* a new frame: frame_buf may have changed */
  const int rm = active_line < (uint32_t)out_lines ? row_map[active_line] : NO_ROW;
  if (rm == NO_ROW) {
    hdmi_fill(dst, words, 0);                           /* (sides black too: border_buf stays right) */
    return;
  }
  const int n = pic_words, pad = pic_pad;
  bool sides = false;
  for (int i = 0; i < border_n; i++) if (border_buf[i] == dst) { sides = true; break; }
  if (!sides) {
    hdmi_fill(dst, pad, 0);
    hdmi_fill(dst + pad + n, words - pad - n, 0);
    if (border_n < 4) border_buf[border_n++] = dst;
  }
  const int r = rm & 0x7FFF;
  const uint32_t *a = hdmi_row(r, -1);
  uint32_t *d = dst + pad;
  if (rm & ROW_BLEND) {
    const uint32_t *b = hdmi_row(r + 1, r);
    int w = 0;                                        /* 4 words a pass (fewer loop steps) */
    for (; w + 4 <= n; w += 4) {
      const uint32_t a0 = a[w], a1 = a[w + 1], a2 = a[w + 2], a3 = a[w + 3];
      const uint32_t b0 = b[w], b1 = b[w + 1], b2 = b[w + 2], b3 = b[w + 3];
      d[w] = hdmi_mean(a0, b0); d[w + 1] = hdmi_mean(a1, b1);
      d[w + 2] = hdmi_mean(a2, b2); d[w + 3] = hdmi_mean(a3, b3);
    }
    for (; w < n; w++) d[w] = hdmi_mean(a[w], b[w]);
  } else {
    hdmi_copy(d, a, n);
  }
  /* the overlay box (FPS / Scale / Width), top left, each pixel 2 x 2; on
   * the other lines its columns are made black where they are side band
   * (a driver line buffer may still hold the box from an earlier line) */
  if (ov_show) {
    const int ol = (int)active_line - OV_Y;
    if (ol >= 0 && ol < 2 * OV_H) {
      const uint16_t *s = ov_front + (ol >> 1) * OV_W;
      uint32_t *o = dst + OV_X;
      for (int x = 0; x < OV_W; x++) { const uint32_t p = s[x]; o[x] = p | (p << 16); }
    } else if (OV_X < pad) {
      hdmi_fill(dst + OV_X, (OV_X + OV_W < pad ? OV_W : pad - OV_X), 0);
    }
  }
}

#pragma GCC pop_options

/* One axis's map: out output pixels / lines from n source ones starting at
 * first, each from the source one under its centre (Drop), or the mean of
 * two where it straddles them with at least a quarter in each (blend). */
static void hdmi_axis(uint16_t *map, int out, int n, int first, int src_len, bool blend, uint16_t flag) {
  for (int i = 0; i < out; i++) {
    const int start = i * n, end = start + n;           /* in 1/out source pixels */
    const int a = start / out, bound = (a + 1) * out;
    int v = (2 * start + n) / (2 * out);                /* under the centre */
    bool mix = false;
    if (blend && end > bound) {                         /* straddles a and a + 1 */
      const int in_a = bound - start, in_b = end - bound;
      if (4 * in_a >= n && 4 * in_b >= n) { v = a; mix = true; }
    }
    v += first;
    if (v >= src_len) v = src_len - 1;
    if (mix && v + 1 >= src_len) mix = false;
    map[i] = (uint16_t)(v | (mix ? flag : 0));
  }
}

/* Width step -> output pixels for the mode running: the 480-line width in
 * proportion to its lines, snapped to a width with a direct path (372, 448,
 * 560, 672) when one is within 10 % and fits, else even and at most the
 * line (the column map). 720x576: 448 / 560 / 672 / 672; 640x480: 372 /
 * 448 / 560 / 560; 720x400: 310 / 372 / 448 / 560.
 * (Since: odd steps are "zoomed out", not cropped - the user found the crop
 * (the middle 240 rows, 20 % taller) zoomed the wrong way: the whole game,
 * smaller, at an exact height - hdmi_size.) */
static const int HDMI_FAST[5] = { 336, 372, 448, 560, 672 };   /* widths with a direct path */
static int hdmi_snap(int px, int max) {
  int best = -1;
  if (px >= max) {                                      /* doesn't fit: the widest direct one that does */
    for (int i = 0; i < 5; i++) if (HDMI_FAST[i] <= max) best = HDMI_FAST[i];
    if (best > 0) return best;
    px = max;
  }
  for (int i = 0; i < 5; i++)
    if (HDMI_FAST[i] <= max && 10 * abs(HDMI_FAST[i] - px) <= px && (best < 0 || abs(HDMI_FAST[i] - px) < abs(best - px)))
      best = HDMI_FAST[i];
  return best > 0 ? best : (px & ~1);
}

/* Width step -> the picture's size in the mode running. Even steps: full
 * height, widths 372 / 448 / 560 / 672 at 480 lines (in proportion
 * elsewhere). Odd steps ("zoomed out"): the whole game smaller, at the
 * largest exact height below full - a multiple of 144 (1.5x = 432 at 480
 * and 576 lines, 2x = 576 at 600, 1x = 288 at 400) - and widths 336 (the
 * true shape: 1.5x both ways at 432) / 448 / 560 / 672 in proportion;
 * black all round. */
static void hdmi_size(int step, int *px_out, int *lines_out) {
  static const int OUT_W[4] = { 336, 448, 560, 672 };   /* at 432 lines */
  const int full = out_lines < HDMI_MAX_LINES ? out_lines : HDMI_MAX_LINES;
  const int max = 2 * out_words < HDMI_MAX_PX ? 2 * out_words : HDMI_MAX_PX;
  int lines = full, px;
  if (step & 1) {
    lines = 0;
    for (int h = 288; h < full; h += 144) lines = h;
    if (lines == 0) lines = full;
    px = OUT_W[(step >> 1) & 3] * lines / 432;
  } else {
    px = HDMI_WIDTHS[(step >> 1) & 3] * lines / 480;
  }
  *px_out = hdmi_snap(px, max);
  *lines_out = lines;
}
static int hdmi_width(int step) { int px, l; hdmi_size(step, &px, &l); return px; }
static int hdmi_height(int step) { int px, l; hdmi_size(step, &px, &l); return l; }

/* The game picture's maps for the mode just started: full height, square
 * pixels (224 x 288 scaled the same both ways), centred across.
 * (Since: for the Scale / Width settings; core 0, in the vertical blank -
 * Video::write's first strip of a frame - or before the game picture. And
 * the zoomed-out steps: fewer lines, centred down too.) */
static void hdmi_geometry(void) {
  const int mode = g_scale_mode, step = (g_stretch >= 0 && g_stretch < 8) ? g_stretch : 0;
  const int lines = out_lines < HDMI_MAX_LINES ? out_lines : HDMI_MAX_LINES;
  int px, pic_lines;
  hdmi_size(step, &px, &pic_lines);
  const int top = (lines - pic_lines) / 2;
  hdmi_axis(col_map, px, GAME_W, 0, GAME_W, mode == SCALE_SMOOTH, COL_BLEND);
  for (int l = 0; l < HDMI_MAX_LINES; l++) row_map[l] = NO_ROW;
  hdmi_axis(row_map + top, pic_lines, GAME_H, 0, GAME_H, mode != SCALE_DROP, ROW_BLEND);
  pic_words = px / 2;
  pic_pad = (out_words - pic_words) / 2;
  /* the direct paths (hdmi_make_row): 372 = 5:3, 448 = 2x, 560 = 2.5x,
   * 672 = 3x, 336 = 1.5x across */
  pic_fast = px == 372 ? (mode == SCALE_SMOOTH ? 3 : 1) : px == 2 * GAME_W ? 2
           : px == 560 ? (mode == SCALE_SMOOTH ? 5 : 4) : px == 672 ? 6
           : px == 336 ? (mode == SCALE_SMOOTH ? 8 : 7) : 0;
  pic_mode = mode;
  pic_step = step;
  border_n = 0;                                         /* the sides moved */
  row_buf_r[0] = row_buf_r[1] = -1;
}

static void hdmi_pal_332(void) {
  for (int i = 0; i < 256; i++) {
    uint32_t r = (i >> 5) & 7, g = (i >> 2) & 7, b = i & 3;
    uint32_t c = ((r * 31 / 7) << 11) | ((g * 63 / 7) << 5) | (b * 31 / 3);
    hdmi_pal[i] = c | (c << 16);
  }
}

#ifdef A8_HDMI_AUDIO
/* ---- sound over HDMI ------------------------------------------------------
 * As Pico2Atari8's a8_display.cpp: the driver sends 16-bit stereo in data
 * islands in the blanking (its dvhstx_audio.cpp and pico_hdmi's packet code,
 * compiled at the end of this file and in pico_freertos_stubs.c), from a
 * queue of encoded packets (36 KB, in RAM). pico_audio.cpp puts the game's
 * samples into its HDMI ring instead of I2S (Sound row / serial key o); the
 * feed here takes them, resamples them to 48000 Hz (= exactly 2 x Galagino's
 * 24000; Donkey Kong's 11765 interpolated) and keeps the queue
 * HDMI_AUDIO_TARGET packets deep. On core 0 - the HDMI core here (Pico2Atari8:
 * core 1, its HDMI core) - from the frame wait and between the render strips. The output
 * always starts with sound (hdmi_start). */
extern "C" {
#include "drivers/dvhstx/audio_config.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/hstx_data_island_queue.h"
}
#include "pico_audio.h"
static const uint32_t HDMI_AUDIO_RATE = 48000;
static const uint32_t HDMI_AUDIO_TARGET = 200;          /* packets queued (4 frames each): 16.7 ms */
static volatile bool hdmi_audio_on = false;
static int hdmi_audio_frame = 0;                        /* the packets' frame counter (channel status) */
static int16_t ha_prev = 0, ha_cur = 0;                 /* the two input samples around the output one */
static uint32_t ha_pos = 0;                             /* its place between them, 16.16 */

/* Core 0: top the queue up from the HDMI ring. */
static void __no_inline_not_in_flash_func(hdmi_audio_feed)(void) {
  if (!hdmi_audio_on) return;
  const int rate = pico_audio_hdmi_rate();
  if (rate <= 0) return;
  const uint32_t step = (uint32_t)(((uint64_t)rate << 16) / HDMI_AUDIO_RATE);
  const bool in_hsync = hstx_di_queue_get_hsync_active();
  while (hstx_di_queue_get_level() < HDMI_AUDIO_TARGET) {
    /* the input samples 4 output ones need: all there, or wait for them */
    const uint32_t need = (ha_pos + 4 * step) >> 16;
    if (pico_audio_hdmi_avail() < need) return;         /* nothing due: the driver sends silence */
    audio_sample_t f[4];
    for (int k = 0; k < 4; k++) {
      while (ha_pos >= 0x10000u) {
        int16_t s;
        pico_audio_hdmi_take(&s, 1);
        ha_prev = ha_cur;
        ha_cur = s;
        ha_pos -= 0x10000u;
      }
      const int32_t v = ha_prev + (((int32_t)ha_cur - ha_prev) * (int32_t)ha_pos >> 16);
      f[k].left = f[k].right = (int16_t)v;
      ha_pos += step;
    }
    hstx_packet_t packet;
    const int next = hstx_packet_set_audio_samples_cs_rate(&packet, f, 4, hdmi_audio_frame, HDMI_AUDIO_RATE);
    hstx_data_island_t island;
    hstx_encode_data_island(&island, &packet, false, in_hsync);
    if (!hstx_di_queue_push(&island)) return;          /* (below the target: has room) */
    hdmi_audio_frame = next;
  }
}

/* The game list (pico_input.cpp buttons_get): the feed, as the frame wait does in a game. */
extern "C" void video_hdmi_audio_poll(void) { hdmi_audio_feed(); }

/* pico_audio.cpp: is the HDMI output carrying sound (before it starts: it
 * will). */
bool a8_display_hdmi_audio(void) { return hdmi_started ? hdmi_audio_on : true; }
uint32_t a8_display_hdmi_audio_gaps(void) { return hstx_di_queue_silence_count; }
#else
static inline void hdmi_audio_feed(void) {}
#endif

/* Starts the output (Video::begin, core 0). The driver's interrupt runs on
 * the core that starts it: core 0, the renderer's, since the USB host moved
 * to core 1 next to the emulator (Pico2Atari8's split). (Was video_core1_poll,
 * called by setup1 on core 1: the line interrupt took 46 %, then 15 %, of the
 * emulator's core.) */
static void hdmi_start(int m) {
  /* the Fruit Jam's HSTX pins (its variant): CK+ 13, D0+ 15, D1+ 17, D2+ 19 */
  static const pimoroni::DVHSTXPinout pins = { PIN_CKP, { PIN_D0P, PIN_D1P, PIN_D2P } };
  if (m < 0 || m >= HDMI_NMODES) m = (PICO_HDMI_MODE >= 0 && PICO_HDMI_MODE < HDMI_NMODES) ? PICO_HDMI_MODE : 5;
  if (hdmi_started) hdmi.reset();                       /* a mode change (Screen row) */
  hdmi_vmode = m;
  hdmi.set_scanline_callback(hdmi_line);
#ifdef A8_HDMI_AUDIO
  /* Always with sound (data islands; silent while the sound goes to the
   * codec): switching the sound to HDMI then restarts nothing (Pico2Atari8:
   * restarting DVI <-> HDMI lost the picture on a display). A mode with no
   * room for it: picture only. */
  hdmi.enable_audio(HDMI_AUDIO_RATE);
  hdmi_audio_frame = 0;
  hdmi_ok = hdmi.init(HDMI_MODES[m].w, HDMI_MODES[m].h, pimoroni::DVHSTX::MODE_RGB565, false, pins);
  hdmi_audio_on = hdmi_ok;
  if (!hdmi_ok) {
    Serial.printf("[video] HDMI sound doesn't fit %s: picture only\n", HDMI_MODES[m].name);
    hdmi.enable_audio(0);
    hdmi_ok = hdmi.init(HDMI_MODES[m].w, HDMI_MODES[m].h, pimoroni::DVHSTX::MODE_RGB565, false, pins);
  }
#else
  hdmi_ok = hdmi.init(HDMI_MODES[m].w, HDMI_MODES[m].h, pimoroni::DVHSTX::MODE_RGB565, false, pins);
#endif
  if (hdmi_ok) {
    out_words = hdmi.get_width() / 2;
    out_lines = hdmi.get_height();
  }
  hdmi_geometry();
  __dmb();
  hdmi_started = true;
}

/* Core 0, Video::begin: the canvas, then the output. */
static void hdmi_begin(void) {
  canvas.setColorDepth(8);
  canvas8 = canvas.attach((uint8_t *)frame_buf, CANVAS_W, CANVAS_H);   /* (no heap: frame_buf's start) */
  canvas.set_screen_size(CANVAS_W, CANVAS_H);   /* fillScreen(): all of it */
  canvas.fillSprite(TFT_BLACK);
  hdmi_pal_332();
  __dmb();
  hdmi_wanted = true;
  {
    /* Button 3 (GPIO 5, low when pressed; an input with pull-up by now)
     * held at power-on: the default mode, for a display that doesn't take
     * the saved one (as Pico2Atari8; button 1 is BOOT). */
    if (digitalRead(5) == LOW && g_hdmi_mode >= 0 && g_hdmi_mode != video_hdmi_mode_default()) {
      Serial.println("[video] button 3 held: the default screen mode");
      g_hdmi_mode = -1;
      pico_config_save_hdmi_mode(-1);
    }
  }
  hdmi_start(g_hdmi_mode);
  const int m = hdmi_vmode;
  char funcs[9];
  for (int i = 0; i < 8; i++) funcs[i] = (char)('0' + (gpio_get_function(12 + i) & 0x1F) % 10);
  funcs[8] = 0;
  Serial.printf("[video] HDMI %s %s (%d x %d lines; game %d x %d at word %d), sys %lu MHz, "
                "clk_hstx %lu Hz, HSTX csr %08lx, GPIO 12-19 functions %s (0 = HSTX)\n",
                HDMI_MODES[m].name, !hdmi_started ? "NOT STARTED (core 1)" : hdmi_ok ? "running" : "FAILED",
                out_words * 2, out_lines, pic_words * 2, out_lines, pic_pad,
                (unsigned long)(clock_get_hz(clk_sys) / 1000000),
                (unsigned long)clock_get_hz(clk_hstx), (unsigned long)hstx_ctrl_hw->csr, funcs);
}

/* Core 0, the game's first strip: the output goes black (canvas8 = null),
 * the canvas lets go of frame_buf once the line interrupt is off it (two
 * frames), frame_buf is cleared and the lines come from it. */
static uint32_t ov_until = 0;                           /* the overlay box: Scale / Width shown alone until */

static void hdmi_game_start(void) {
  canvas8 = nullptr;
  __dmb();
  const uint32_t f0 = hdmi.get_frame_count();
  for (uint32_t t0 = millis(); hdmi.get_frame_count() - f0 < 2 && millis() - t0 < 100; ) delay(1);
  canvas.detach();
  memset(frame_buf, 0, sizeof(frame_buf));
  __dmb();
  hdmi_game = true;
  ov_until = millis() + 3000;                         /* the Scale / Width shown at the start */
  Serial.printf("[video] HDMI: game picture (heap free %d)\n", rp2040.getFreeHeap());
}

/* Core 0, galagino.cpp update_screen: wait for the next HDMI frame. The
 * driver counts a frame where its vertical blank starts (and sends an event,
 * __sev, then), so the frame rendered after this is in frame_buf before the
 * beam gets to it. 50 ms at most (no output: don't hang the game). */
void video_hdmi_wait_vblank(void) {
  static uint32_t last = 0;
  const uint32_t t0 = time_us_32();
  uint32_t f;
  while ((f = hdmi.get_frame_count()) == last && time_us_32() - t0 < 50000) {
    hdmi_audio_feed();                          /* (HDMI sound: keep its queue topped up meanwhile) */
    __wfe();
  }
  last = f;
}

/* galagino.cpp update_screen, once per HDMI frame: how many 60 Hz game frames
 * to run for it - 1 at 60 Hz; at 50 Hz now and then 2, at 70 Hz now and then
 * 0 - so the game keeps its speed in every mode (a frame is shown twice, or
 * one isn't shown). */
/* galagino.cpp update_screen: tile row row (8 lines) of frame_buf to render
 * straight into once the game picture is up (else null: the strip buffer, and
 * Video::write copies it) - saves 36 strip copies a frame on core 0. */
unsigned short *video_hdmi_strip(int row) {
  return (hdmi_game && row >= 0 && row < GAME_H / 8) ? &frame_buf[row * 8 * GAME_W] : nullptr;
}

int video_hdmi_emu_frames(void) {
  static uint32_t acc = 0;
  const uint32_t hz = HDMI_MODES[hdmi_vmode].hz;
  acc += 60;
  const int n = (int)(acc / hz);
  acc -= (uint32_t)n * hz;
  return n;
}

/* ---- the Screen row (pico_menu.cpp, core 0, before a game) --------------- */
extern "C" int video_hdmi_modes(void) { return HDMI_NMODES; }
extern "C" int video_hdmi_mode(void) { return hdmi_vmode; }
extern "C" int video_hdmi_mode_default(void) {
  return (PICO_HDMI_MODE >= 0 && PICO_HDMI_MODE < HDMI_NMODES) ? PICO_HDMI_MODE : 5;
}
extern "C" const char *video_hdmi_mode_name(int m) {
  return m >= 0 && m < HDMI_NMODES ? HDMI_MODES[m].name : "?";
}
/* Switch the output (the menus' canvas stays); false if it didn't start. */
extern "C" bool video_hdmi_set_mode(int m) {
  hdmi_start(m);
  Serial.printf("[video] HDMI mode %s: %s (%d x %d)\n", HDMI_MODES[hdmi_vmode].name,
                hdmi_ok ? "running" : "FAILED", out_words * 2, out_lines);
  return hdmi_ok;
}
/* The Width row's label: the pixels of width step for the mode running. */
extern "C" int video_hdmi_width(int step) { return hdmi_width(step); }
extern "C" int video_hdmi_height(int step) { return hdmi_height(step); }   /* (zoomed-out steps: below full) */

/* ---- the overlay box and the Scale / Width changes (core 0) -------------- */
extern "C" volatile int g_show_fps;                     /* pico_config.cpp: the Show FPS row */
static TFT_eSprite ov_spr(&tft);                        /* 16-bit, OV_W x OV_H (heap, 5.5 KB) */
static float ov_fps = 0, ov_load = 0;
static bool ov_have_fps = false;
static uint32_t cfg_save_at = 0;                        /* save Scale / Width then (0: nothing to) */
static const char *const SCALE_NAMES[3] = { "Smooth", "Sharp", "Drop" };

/* Serial keys m / k (pico_input.cpp): m = the next screen mode - in the game
 * list the Screen row's FIRE (pico_menu.cpp takes it); in a game at once, for
 * 10 s, kept with k (saved), else back to the one before. */
static volatile bool mode_key = false, keep_key = false;
static int trial_old = -1;                              /* in a game: the mode before the trial */
static uint32_t trial_until = 0;                        /* the trial's end (0: none) */
static int trial_left_shown = -1;
extern "C" void video_hdmi_mode_key(void) { mode_key = true; }
extern "C" void video_hdmi_keep_key(void) { keep_key = true; }
extern "C" bool video_hdmi_take_mode_key(void) {        /* the game list */
  const bool k = mode_key;
  mode_key = false;
  keep_key = false;
  return k;
}

/* Draw the box into the back buffer and swap it in. */
static void hdmi_overlay_draw(void) {
  if (!ov_spr.getPointer()) {
    ov_spr.setColorDepth(16);
    if (!ov_spr.createSprite(OV_W, OV_H)) return;
  }
  ov_spr.fillSprite(TFT_BLACK);
  ov_spr.setTextSize(1);
  int y = 2;
  if (g_show_fps && ov_have_fps) {
    ov_spr.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    ov_spr.setCursor(2, y); ov_spr.printf("%4.1f fps", ov_fps); y += 10;
    ov_spr.setTextColor(ov_load < 80.0f ? TFT_GREEN : ov_load < 95.0f ? TFT_YELLOW : TFT_RED, TFT_BLACK);
    ov_spr.setCursor(2, y); ov_spr.printf("CPU %3.0f%%", ov_load); y += 10;
  }
  const int step = pic_step < 0 ? 0 : pic_step;
  if (trial_until) {                                    /* a screen mode on trial (key m) */
    const int left = (int)((int32_t)(trial_until - millis()) + 999) / 1000;
    ov_spr.setTextColor(TFT_YELLOW, TFT_BLACK);
    ov_spr.setCursor(2, y); ov_spr.print(HDMI_MODES[hdmi_vmode].name); y += 10;
    ov_spr.setCursor(2, y); ov_spr.printf("k: keep %2d s", left > 0 ? left : 0);
    trial_left_shown = left;
  } else {
  ov_spr.setTextColor(TFT_CYAN, TFT_BLACK);
  ov_spr.setCursor(2, y); ov_spr.print(SCALE_NAMES[(pic_mode >= 0 && pic_mode < 3) ? pic_mode : 0]); y += 10;
  if (step & 1) { ov_spr.setCursor(2, y); ov_spr.printf("%dx%d", hdmi_width(step), hdmi_height(step)); }   /* zoomed out */
  else           { ov_spr.setCursor(2, y); ov_spr.printf("%d", hdmi_width(step)); }
  }
  const uint16_t *src = (const uint16_t *)ov_spr.getPointer();   /* byte-swapped (the sprite's) */
  uint16_t *back = (ov_front == ov_buf[0]) ? ov_buf[1] : ov_buf[0];
  for (int i = 0; i < OV_W * OV_H; i++) back[i] = __builtin_bswap16(src[i]);
  __dmb();
  ov_front = back;
}

/* Once a second (fps_stats_tick): the numbers, if Show FPS is on. */
static void hdmi_overlay_fps(float scr_fps, float emu_fps, float load) {
  (void)scr_fps;
  ov_fps = emu_fps;
  ov_load = load;
  ov_have_fps = true;
  if (ov_show && g_show_fps) hdmi_overlay_draw();
}

/* Each frame's first strip (Video::write, in the vertical blank): a changed
 * Scale / Width (game list rows, serial keys z / v) gets new maps and is shown
 * for 3 s, and saved 3 s after the last change - a flash write power-cycles
 * the USB ports, so not on every key; then the box on or off. */
static void hdmi_frame_start(void) {
  const uint32_t now = millis();
  if (g_scale_mode != pic_mode || g_stretch != pic_step) {
    hdmi_geometry();
    ov_until = now + 3000;
    cfg_save_at = now + 3000;
    if (ov_show) hdmi_overlay_draw();
  }
  if (cfg_save_at && (int32_t)(now - cfg_save_at) >= 0) {
    cfg_save_at = 0;
    pico_config_save_scale(g_scale_mode);       /* no-op if unchanged */
    pico_config_save_stretch(g_stretch);
  }
  /* screen mode keys (m / k): the next mode now, on trial for 10 s */
  if (mode_key) {
    mode_key = false;
    if (!trial_until) trial_old = hdmi_vmode;
    video_hdmi_set_mode((hdmi_vmode + 1) % HDMI_NMODES);
    trial_until = now + 10000;
    if (!trial_until) trial_until = 1;
    ov_until = trial_until;
    keep_key = false;
    if (ov_show) hdmi_overlay_draw();
  }
  if (trial_until) {
    const int left = (int)((int32_t)(trial_until - now) + 999) / 1000;
    if (keep_key) {
      keep_key = false;
      trial_until = 0;
      pico_config_save_hdmi_mode(hdmi_vmode);
      Serial.printf("[video] screen mode %s kept\n", HDMI_MODES[hdmi_vmode].name);
      ov_until = now + 3000;
      if (ov_show) hdmi_overlay_draw();
    } else if (left <= 0) {
      trial_until = 0;
      video_hdmi_set_mode(trial_old);
      ov_until = now + 3000;
      if (ov_show) hdmi_overlay_draw();
    } else if (left != trial_left_shown && ov_show) {
      hdmi_overlay_draw();                      /* the countdown */
    }
  } else {
    keep_key = false;
  }
  const bool want = g_show_fps || (ov_until && (int32_t)(ov_until - now) > 0);
  if (!want) ov_until = 0;
  if (want && !ov_show) {
    hdmi_overlay_draw();
    ov_show = true;
  } else if (!want && ov_show) {
    ov_show = false;
    ov_clear = true;                            /* the line interrupt makes the sides black again */
  }
}
#endif  /* PICO_HDMI */

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

#ifdef PICO_HDMI
/* HDMI (Fruit Jam): no panel — TFT_eSPI is never started, so its build-flag
 * pins (Fruit Jam button 3, the ESP32's UART) are never touched. */
void Video::begin(void) {
  hdmi_begin();
  cur_row = 0;
}
#else
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
#endif  /* !PICO_HDMI */

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
#ifdef PICO_HDMI
  if (!hdmi_game) hdmi_game_start();                  /* the game's first strip: frame_buf is its now */
  if (cur_row == 0) hdmi_frame_start();               /* Scale / Width, the overlay box */
  hdmi_audio_feed();                                  /* HDMI sound: between the render strips too */
#endif
#ifdef PICO_HDMI
  if (colors != &frame_buf[cur_row * GAME_W])        /* (rendered in place: video_hdmi_strip) */
#endif
  memcpy(&frame_buf[cur_row * GAME_W], colors, (size_t)rows * GAME_W * 2);

  cur_row += rows;
  if (cur_row >= GAME_H) {
#if defined(PICO_HDMI)
    /* nothing to push: the HDMI lines are read from frame_buf as they go out */
#elif defined(PICO_TINY_SCREEN)
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
#ifdef PICO_HDMI
  {
    /* XIP cache (shared by both cores) over the last second: accesses and
     * hits (the counters clear on write). Diagnosis of the emulator's speed. */
    const uint32_t acc = xip_ctrl_hw->ctr_acc, hit = xip_ctrl_hw->ctr_hit;
    xip_ctrl_hw->ctr_acc = 0;
    xip_ctrl_hw->ctr_hit = 0;
    Serial.printf("[video] XIP %lu k accesses/s, %lu k misses/s (%.1f%% hit), sys %lu MHz\n",
                  (unsigned long)(acc / 1000), (unsigned long)((acc - hit) / 1000),
                  acc ? 100.0 * hit / acc : 0.0, (unsigned long)(clock_get_hz(clk_sys) / 1000000));
    /* the line interrupt on core 0 (at first core 1): its time, lines and output
     * frames this second (60 frames = no stream restarts), and core 0's render
     * time per frame (galagino.cpp update_screen) */
    static uint32_t last_hdmi_frames = 0;
    const uint32_t us = hdmi_irq_us, lines = hdmi_irq_lines, fr = hdmi.get_frame_count();
    hdmi_irq_us = 0;
    hdmi_irq_lines = 0;
    Serial.printf("[video] HDMI line IRQ %lu us/s (%.1f%% of core 0), %lu lines, %lu frames, render %lu us/frame\n",
                  (unsigned long)us, us / 10000.0, (unsigned long)lines,
                  (unsigned long)(fr - last_hdmi_frames),
                  (unsigned long)(g_hdmi_render_n ? g_hdmi_render_us / g_hdmi_render_n : 0));
    g_hdmi_render_us = g_hdmi_render_n = 0;
    last_hdmi_frames = fr;
  }
#endif
#ifdef PICO_HDMI
  (void)shown;                 /* (HDMI: the overlay box, hdmi_overlay_fps) */
  hdmi_overlay_fps(scr_fps, emu_fps, emu_ms / FRAME_BUDGET_MS * 100.0f);
  if (0) {
#else
  if (g_show_fps) {
#endif
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

#ifdef PICO_HDMI
/* The driver's code, compiled here (last: its file-level names stay out of
 * the code above), as in Pico2Atari8's a8_display.cpp. platformio.ini fetches
 * Adafruit-DVI-HSTX but keeps it out of the build — USE_TINYUSB links every
 * object of a library whole, and the library's Arduino wrapper needs Adafruit
 * GFX — so only Pimoroni's dvhstx driver is built: not its HDMI audio
 * (dvhstx_audio.cpp), as nothing enables it yet. The text-mode font isn't
 * used here, but init() refers to it. (In this file, not one of its own: in
 * Pico2Atari8 even an empty extra object file reordered the linker's veneers
 * in the other builds.) */
#include "drivers/dvhstx/dvhstx.cpp"
/* 800x600 @ 60 Hz by the VESA book: the driver's has a front porch of 44
 * (lines of 1060 pixels, not 1056); Pico2Atari8's first display showed 7 thin
 * vertical bars with it, a second one shows 40 clean (2026-11-11). The
 * driver's table entry is renamed out of the way and this one takes its
 * name. */
#define dvi_timing_800x600p_60hz dvi_timing_800x600p_60hz_driver
#include "drivers/dvhstx/dvi.cpp"
#undef dvi_timing_800x600p_60hz
const struct dvi_timing dvi_timing_800x600p_60hz = {
    .h_sync_polarity = false,
    .h_front_porch   = 40,
    .h_sync_width    = 128,
    .h_back_porch    = 88,
    .h_active_pixels = 800,

    .v_sync_polarity = false,
    .v_front_porch   = 1,
    .v_sync_width    = 4,
    .v_back_porch    = 23,
    .v_active_lines  = 600,

    .bit_clk_khz     = 400000
};
#include "drivers/dvhstx/font_cp437.c"
#ifdef A8_HDMI_AUDIO
#undef NUM_CHANS                         /* dvhstx.cpp's 3; the audio code's is 4 */
#include "drivers/dvhstx/dvhstx_audio.cpp"
#endif
#endif  /* PICO_HDMI */
