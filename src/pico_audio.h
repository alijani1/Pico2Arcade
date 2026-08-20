/* pico_audio.h — Streaming PWM audio for PicoGalagino.
 *
 * Wraps earlephilhower framework's PWMAudio class (DMA-fed PWM output on
 * GP22, gated by amp-enable on GP4). Exposes a C-linkage push API so the
 * i2s_write stub can forward Galagino's PCM stream here.
 *
 * All samples are signed 16-bit mono. Volume is a 0..16 linear scaler
 * (0 = mute, 16 = unity gain). Buffered internally so the caller can
 * push in whatever chunk size Galagino produces.
 */

#ifndef PICO_GALAGINO_AUDIO_H
#define PICO_GALAGINO_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize PWM audio. Safe to call once at boot. If already initialized,
 * this is a no-op unless the sample rate changed. */
void pico_audio_init(int sample_rate);

/* Change sample rate at runtime (Galagino does this when the game changes —
 * DKong wants 11765 Hz, Pac-Man/Galaga want 24000 Hz). */
void pico_audio_set_sample_rate(int sample_rate);

/* Push signed-16 samples into the DMA ring. Returns bytes actually
 * accepted; if the ring is full, returns less than requested (or 0).
 * Galagino's snd_transmit uses this "wrote everything" vs "no room"
 * signal to decide whether to generate more audio. */
size_t pico_audio_push(const void *data, size_t bytes);

/* Volume scaler 0..16 (0 = mute, 16 = unity). */
void    pico_audio_set_volume(uint8_t vol);
void    pico_audio_amp_on(void);
void    pico_audio_amp_off(void);
uint8_t pico_audio_get_volume(void);

#ifdef __cplusplus
}
#endif

#endif
