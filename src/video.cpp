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
struct HdmiMode { uint16_t w, h; const char *name; };
static const HdmiMode HDMI_MODES[] = {
  { 320, 240, "640x480 60 Hz" },
  { 360, 200, "720x400 70 Hz" },
  { 360, 240, "720x480 60 Hz" },
  { 360, 288, "720x576 50 Hz" },
  { 400, 300, "800x600 60 Hz" },
  { 400, 240, "800x480 60 Hz" },
};
static const int HDMI_NMODES = sizeof(HDMI_MODES) / sizeof(HDMI_MODES[0]);
#ifndef PICO_HDMI_MODE
#define PICO_HDMI_MODE 5                                /* index into HDMI_MODES: 800x480 @ 60 Hz */
#endif

static volatile bool hdmi_wanted = false, hdmi_started = false, hdmi_ok = false;
static volatile bool hdmi_game = false;                 /* lines from frame_buf, else the canvas */
static int out_words = 400, out_lines = 480;            /* the mode's line: words (2 pixels), lines */

/* The game picture: pic_words words from word pic_pad of each line. */
static const int HDMI_MAX_PX = 800, HDMI_MAX_LINES = 600;
static const uint16_t NO_ROW = 0xFFFF;
static int pic_words = 0, pic_pad = 0;
static uint8_t  col_map[HDMI_MAX_PX];                   /* output pixel -> source column */
static uint16_t row_map[HDMI_MAX_LINES];                /* output line -> source row, NO_ROW = black */
static uint32_t line_cache[HDMI_MAX_PX / 2];            /* the last line made */
static int cache_row = -1;

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
 * Other heights: the column map, as before. */
static int pic_fast = 0;                                /* 1: 5:3 (480 lines), 2: 2x (576), 0: col_map */
static uint32_t *border_buf[4];                         /* line buffers whose sides are black (game) */
static int border_n = 0;
static bool border_game = false;                        /* border_buf is for the game picture */

static inline __attribute__((always_inline)) uint32_t hdmi_rev16(uint32_t v) {
  uint32_t r;
  __asm__("rev16 %0, %1" : "=r"(r) : "r"(v));
  return r;
}

static inline __attribute__((always_inline)) void hdmi_copy(uint32_t *d, const uint32_t *s, int n) {
  for (; n >= 8; n -= 8, d += 8, s += 8) {
    const uint32_t a = s[0], b = s[1], c = s[2], e = s[3], f = s[4], g = s[5], h = s[6], i = s[7];
    d[0] = a; d[1] = b; d[2] = c; d[3] = e; d[4] = f; d[5] = g; d[6] = h; d[7] = i;
  }
  for (; n > 0; n--) *d++ = *s++;
}

/* Source row r into line_cache (pic_words words). */
static inline __attribute__((always_inline)) void hdmi_make_row(int r) {
  const uint16_t *s = frame_buf + r * GAME_W;
  uint32_t *o = line_cache;
  if (pic_fast == 1) {
    const uint32_t *w = (const uint32_t *)s;            /* 2 pixels a word (rows start word-aligned) */
    for (int g = 0; g < GAME_W / 6; g++, w += 3, o += 5) {
      const uint32_t a = hdmi_rev16(w[0]), b = hdmi_rev16(w[1]), c = hdmi_rev16(w[2]);   /* s0|s1, s2|s3, s4|s5 */
      o[0] = (a & 0xFFFFu) | (a << 16);
      o[1] = (a >> 16) | (a & 0xFFFF0000u);
      o[2] = b;
      o[3] = (b >> 16) | (c << 16);
      o[4] = c;
    }
    *o = hdmi_rev16(w[0]);                              /* the last 2 of 224: s222 s223 */
  } else if (pic_fast == 2) {
    for (int x = 0; x < GAME_W; x++) {
      const uint32_t p = hdmi_rev16(s[x]) & 0xFFFFu;
      o[x] = p | (p << 16);
    }
  } else {
    const uint8_t *cm = col_map;
    for (int w = 0; w < pic_words; w++, cm += 2) o[w] = hdmi_px2(s[cm[0]], s[cm[1]]);
  }
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
  if (active_line == 0) cache_row = -1;                 /* a new frame: frame_buf may have changed */
  const int r = active_line < (uint32_t)out_lines ? row_map[active_line] : NO_ROW;
  if (r == NO_ROW) {
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
  if (r != cache_row) {
    hdmi_make_row(r);
    cache_row = r;
  }
  hdmi_copy(dst + pad, line_cache, n);
}

#pragma GCC pop_options

/* The game picture's maps for the mode just started: full height, square
 * pixels (224 x 288 scaled the same both ways), centred across. */
static void hdmi_geometry(void) {
  const int lines = out_lines < HDMI_MAX_LINES ? out_lines : HDMI_MAX_LINES;
  int px = (GAME_W * lines / GAME_H) & ~1;
  if (px > 2 * out_words) px = 2 * out_words;
  for (int p = 0; p < px; p++) col_map[p] = (uint8_t)(p * GAME_W / px);
  for (int l = 0; l < HDMI_MAX_LINES; l++)
    row_map[l] = l < lines ? (uint16_t)(l * GAME_H / lines) : NO_ROW;
  pic_words = px / 2;
  pic_pad = (out_words - pic_words) / 2;
  /* the direct paths (hdmi_make_row): 480 lines = 5:3, 576 lines = 2x */
  pic_fast = (lines == 480 && px == 372) ? 1 : (lines == 576 && px == 448) ? 2 : 0;
}

static void hdmi_pal_332(void) {
  for (int i = 0; i < 256; i++) {
    uint32_t r = (i >> 5) & 7, g = (i >> 2) & 7, b = i & 3;
    uint32_t c = ((r * 31 / 7) << 11) | ((g * 63 / 7) << 5) | (b * 31 / 3);
    hdmi_pal[i] = c | (c << 16);
  }
}

/* Starts the output (Video::begin, core 0). The driver's interrupt runs on
 * the core that starts it: core 0, the renderer's, since the USB host moved
 * to core 1 next to the emulator (Pico2Atari8's split). (Was video_core1_poll,
 * called by setup1 on core 1: the line interrupt took 46 %, then 15 %, of the
 * emulator's core.) */
static void hdmi_start(void) {
  /* the Fruit Jam's HSTX pins (its variant): CK+ 13, D0+ 15, D1+ 17, D2+ 19 */
  static const pimoroni::DVHSTXPinout pins = { PIN_CKP, { PIN_D0P, PIN_D1P, PIN_D2P } };
  const int m = (PICO_HDMI_MODE >= 0 && PICO_HDMI_MODE < HDMI_NMODES) ? PICO_HDMI_MODE : 5;
  hdmi.set_scanline_callback(hdmi_line);
  hdmi_ok = hdmi.init(HDMI_MODES[m].w, HDMI_MODES[m].h, pimoroni::DVHSTX::MODE_RGB565, false, pins);
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
  hdmi_start();
  const int m = (PICO_HDMI_MODE >= 0 && PICO_HDMI_MODE < HDMI_NMODES) ? PICO_HDMI_MODE : 5;
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
static void hdmi_game_start(void) {
  canvas8 = nullptr;
  __dmb();
  const uint32_t f0 = hdmi.get_frame_count();
  for (uint32_t t0 = millis(); hdmi.get_frame_count() - f0 < 2 && millis() - t0 < 100; ) delay(1);
  canvas.detach();
  memset(frame_buf, 0, sizeof(frame_buf));
  __dmb();
  hdmi_game = true;
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
  while ((f = hdmi.get_frame_count()) == last && time_us_32() - t0 < 50000) __wfe();
  last = f;
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
  (void)shown;                 /* TODO: an HDMI overlay; the serial line above only */
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
#endif  /* PICO_HDMI */
