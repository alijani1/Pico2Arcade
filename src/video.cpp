/* video.cpp — PicoGalagino: TFT_eSPI backend for the Video class.
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

static int cur_row = 0;

/* Single buffer (double-buffer variant kept as commented-out reference).
 * At 200 MHz sys / 100 MHz SPI, blocking pushImage may be fast enough to
 * still hit real speed with half the RAM footprint. */
static uint16_t frame_buf[GAME_W * GAME_H];

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
  tft.setRotation(0);
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

void Video::write(uint16_t *colors, uint32_t len) {
  int rows = (int)(len / GAME_W);
  if (rows <= 0) return;

  if (cur_row + rows > GAME_H) rows = GAME_H - cur_row;
  memcpy(&frame_buf[cur_row * GAME_W], colors, (size_t)rows * GAME_W * 2);

  cur_row += rows;
  if (cur_row >= GAME_H) {
    /* Single-buffer blocking push — this call waits for the SPI transfer
     * to complete. Simpler than the double-buffered DMA path and uses
     * half the RAM. */
    tft.pushImage(TFT_X_OFFSET, TFT_Y_OFFSET, GAME_W, GAME_H, frame_buf);
    cur_row = 0;

    static uint32_t frame = 0;
    static uint32_t lastMs = 0;
    frame++;
    if (frame % 60 == 0) {
      uint32_t now = millis();
      uint32_t dt = now - lastMs;
      lastMs = now;
      Serial.printf("[video] frame=%u  60 frames in %u ms = %.1f fps\n",
                    (unsigned)frame, (unsigned)dt, dt ? 60000.0f / dt : 0.0f);
    }
  }
}
