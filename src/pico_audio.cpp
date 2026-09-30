/* pico_audio.cpp — PWM audio implementation. */

#include <Arduino.h>
#include <PWMAudio.h>
#include "pico_audio.h"
#include "pico_board.h"

#ifdef PICO_BOARD_TABLE
/* Pins from the runtime table (pico_board.h) — the PWMAudio object is created
 * on first init, once the table is loaded. amp = -1: no enable pin. */
#define AUDIO_PIN  g_pins.audio
#define AMP_EN_PIN g_pins.amp
static PWMAudio *g_audio_p = nullptr;
#define g_audio (*g_audio_p)
#else
/* Wiring — same pads as the PicoCartReader VCS emulator so we can share
 * hardware. GP22 = PWM audio out, GP4 = PAM8302 amp enable (active HIGH). */
static const int AUDIO_PIN = 22;
static const int AMP_EN_PIN = 4;

/* PWMAudio wants pin at construction. Create in mono mode. */
static PWMAudio g_audio(AUDIO_PIN, /*stereo=*/false);
#endif

static bool     g_started    = false;
static int      g_sample_rate = 0;
// Volume 0..100 %, applied as gain = pct² / 10000 — the same perceptual
// curve PicoCartReader uses, so a given % sounds the same in both.
static volatile int g_volume_pct = 100;
static volatile int g_gain       = 10000;   // pct², 0..10000

extern "C" void pico_audio_init(int sample_rate) {
    if (g_started && sample_rate == g_sample_rate) return;

    if (!g_started) {
#ifdef PICO_BOARD_TABLE
        if (AUDIO_PIN < 0) return;                 /* no audio wired */
        g_audio_p = new PWMAudio(AUDIO_PIN, /*stereo=*/false);
#endif
        // 4 buffers × 256 words = 1024 samples. ~42ms of latency at 24kHz;
        // small enough that game→audio lag stays imperceptible, large enough
        // that we survive a few missed refills.
        g_audio.setBuffers(4, 256);
        g_audio.begin(sample_rate);
        if (AMP_EN_PIN >= 0) {
            pinMode(AMP_EN_PIN, OUTPUT);
            digitalWrite(AMP_EN_PIN, LOW);    // amp OFF until game starts
        }
        g_started = true;
    } else {
        // Sample-rate switch mid-session (game change).
        g_audio.end();
        g_audio.begin(sample_rate);
    }
    g_sample_rate = sample_rate;
}

/* Flash writes turn interrupts off for tens of ms — longer than one DMA
 * buffer — and the ping-pong DMA's re-arm IRQ gets missed: the two channels
 * end up out of step for good (constant clicking, and the runaway DMA slows
 * everything down). So audio is stopped cleanly around every flash write and
 * restarted afterwards. Call with the other core idled (it pushes samples). */
extern "C" void pico_audio_suspend(void) {
    if (g_started) g_audio.end();
}
extern "C" void pico_audio_resume(void) {
    if (g_started) g_audio.begin(g_sample_rate);
}

extern "C" void pico_audio_set_sample_rate(int sample_rate) {
    pico_audio_init(sample_rate);
}

extern "C" size_t pico_audio_push(const void *data, size_t bytes) {
    if (!g_started || bytes < 2) return 0;
    // Galagino writes UNSIGNED 16-bit samples centered on 0x8000
    // (see galagino.cpp: snd_buffer[i] = 0x8000 + v). We convert
    // to signed by subtracting the DC bias, then apply volume.
    const uint16_t *samples = (const uint16_t *)data;
    size_t nsamp = bytes / 2;
    size_t written = 0;

    if (g_audio.availableForWrite() < 2) return 0;

    for (size_t i = 0; i < nsamp; i++) {
        int32_t centered = (int32_t)samples[i] - 32768;    // now -32768..32767
        int32_t s        = centered * g_gain / 10000;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        if (g_audio.availableForWrite() < 2) break;
        g_audio.write((int16_t)s);
        written++;
    }
    return written * 2;   // bytes actually written
}

extern "C" void pico_audio_set_volume_pct(int pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    g_volume_pct = pct;
    g_gain       = pct * pct;
}

extern "C" int pico_audio_get_volume_pct(void) {
    return g_volume_pct;
}

// Legacy 0..16 API, mapped onto the % scale.
extern "C" void pico_audio_set_volume(uint8_t vol) {
    if (vol > 16) vol = 16;
    pico_audio_set_volume_pct(vol * 100 / 16);
}

extern "C" uint8_t pico_audio_get_volume(void) {
    return (uint8_t)((g_volume_pct * 16 + 50) / 100);
}

extern "C" void pico_audio_amp_on(void) {
    if (AMP_EN_PIN >= 0) digitalWrite(AMP_EN_PIN, HIGH);
}
extern "C" void pico_audio_amp_off(void) {
    if (AMP_EN_PIN >= 0) digitalWrite(AMP_EN_PIN, LOW);
}
