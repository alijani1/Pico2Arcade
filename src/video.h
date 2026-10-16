/* video.h — Pico2Arcade: TFT_eSPI-backed video driver
 *
 * Replaces upstream Galagino's ESP-IDF spi_master driver. Same public API
 * (begin, write) so the game code compiles unchanged; internals use
 * TFT_eSPI's DMA path (proven on this hardware by the VCS emulator).
 */

#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>

class Video {
public:
  Video();

  // Init the display. Called once from setup().
  void begin(void);

  // Push `len` RGB565 pixels into the current draw window (row-major).
  // The game blits full frames or narrow strips using this.
  void write(uint16_t *colors, uint32_t len);

private:
  /* Once a second: FPS/CPU stats to serial + optional on-screen overlay. */
  void fps_stats_tick(void);
};

#endif // VIDEO_H
