/* driver/i2s.h — Pico2Arcade no-op stub
 *
 * Galagino uses the ESP32's built-in I2S DAC for audio. This stub lets
 * the upstream sketch compile on the Pico with all I2S calls becoming
 * no-ops (audio is silent). Real audio will land later via PWM on GP22,
 * matching the PicoCartReader path. */

#ifndef PICO_GALAGINO_I2S_STUB_H
#define PICO_GALAGINO_I2S_STUB_H

#include <stdint.h>
#include <stddef.h>

typedef int i2s_port_t;
#define I2S_NUM_0 0

typedef enum {
  I2S_MODE_MASTER = 1,
  I2S_MODE_TX     = 2,
  I2S_MODE_DAC_BUILT_IN = 4,
} i2s_mode_t;

typedef enum {
  I2S_BITS_PER_SAMPLE_16BIT = 16,
} i2s_bits_per_sample_t;

typedef enum {
  I2S_CHANNEL_FMT_RIGHT_LEFT = 0,
  I2S_CHANNEL_FMT_ONLY_LEFT  = 1,
  I2S_CHANNEL_FMT_ONLY_RIGHT = 2,
} i2s_channel_fmt_t;

typedef enum {
  I2S_COMM_FORMAT_STAND_MSB = 0,
} i2s_comm_format_t;

typedef enum {
  I2S_DAC_CHANNEL_BOTH_EN  = 0,
  I2S_DAC_CHANNEL_LEFT_EN  = 1,
  I2S_DAC_CHANNEL_RIGHT_EN = 2,
} i2s_dac_mode_t;

typedef struct {
  int mode;
  int sample_rate;
  int bits_per_sample;
  int channel_format;
  int communication_format;
  int intr_alloc_flags;
  int dma_buf_count;
  int dma_buf_len;
  int use_apll;
  int tx_desc_auto_clear;
  int fixed_mclk;
} i2s_config_t;

/* Forward to our streaming PWM driver. Declared inline here so this
 * header stays independent of C++ headers; real symbols live in
 * pico_audio.cpp with extern "C" linkage. */
#ifdef __cplusplus
extern "C" {
#endif
extern void   pico_audio_init(int sample_rate);
extern void   pico_audio_set_sample_rate(int sample_rate);
extern size_t pico_audio_push(const void *data, size_t bytes);
#ifdef __cplusplus
}
#endif

static inline int i2s_driver_install(i2s_port_t, const i2s_config_t *cfg, int, void*) {
  pico_audio_init(cfg ? cfg->sample_rate : 24000);
  return 0;
}
static inline int i2s_set_dac_mode(i2s_dac_mode_t) { return 0; }
static inline int i2s_set_sample_rates(i2s_port_t, int rate) {
  pico_audio_set_sample_rate(rate);
  return 0;
}
/* Forward Galagino's PCM stream to the PWM ring. Report actual bytes
 * accepted; snd_transmit's do..while(bytesOut) exits when the ring is
 * full (bytesOut = 0), which is exactly the pacing we want. */
static inline int i2s_write(i2s_port_t, const void *data, size_t bytes,
                            size_t *bytes_written, int) {
  size_t w = pico_audio_push(data, bytes);
  if (bytes_written) *bytes_written = w;
  return 0;
}

#endif
