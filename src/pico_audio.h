/* pico_audio.h — Streaming PWM audio for Pico2Arcade.
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

/* Stop / restart the PWM audio around flash writes (interrupts off would
 * desynchronise its DMA). Call with the other core idled. */
void pico_audio_suspend(void);
void pico_audio_resume(void);

/* Volume 0..100 % with a perceptual (squared) curve — matches
 * PicoCartReader. */
void    pico_audio_set_volume_pct(int pct);
int     pico_audio_get_volume_pct(void);

/* Legacy volume scaler 0..16 (0 = mute, 16 = unity), mapped onto %. */
void    pico_audio_set_volume(uint8_t vol);
void    pico_audio_amp_on(void);
void    pico_audio_amp_off(void);
uint8_t pico_audio_get_volume(void);

#ifdef PICO_FRUITJAM
/* I2S / TLV320DAC3100 (pico_audio.cpp, as Pico2Atari8's): */
/* From the main loop: follows the headphone jack (auto output). */
void    pico_audio_poll(void);
/* Output: 0 auto (headphones when plugged in, else the speaker),
 * 1 headphones, 2 speaker, 3 both. */
void    pico_audio_set_output(int mode);
/* Log the ring's health since the last call (under-runs, lowest level). */
void    pico_audio_report(void);
/* A key from the serial monitor: + - volume, A H S B output, I report. */
void    pico_audio_key(int c);
/* Output 0 - 3 as pico_audio_set_output (4 HDMI: not yet here). */
void    pico_audio_set_sound_output(int o);
int     pico_audio_get_sound_output(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
