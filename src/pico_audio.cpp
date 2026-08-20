/* pico_audio.cpp — PWM audio implementation. */

#include <Arduino.h>
#include <PWMAudio.h>
#include "pico_audio.h"

/* Wiring — same pads as the PicoCartReader VCS emulator so we can share
 * hardware. GP22 = PWM audio out, GP4 = PAM8302 amp enable (active HIGH). */
static const int AUDIO_PIN = 22;
static const int AMP_EN_PIN = 4;

/* PWMAudio wants pin at construction. Create in mono mode. */
static PWMAudio g_audio(AUDIO_PIN, /*stereo=*/false);

static bool     g_started    = false;
static int      g_sample_rate = 0;
static uint8_t  g_volume     = 16;   // default 100% (max) out of 16

extern "C" void pico_audio_init(int sample_rate) {
    if (g_started && sample_rate == g_sample_rate) return;

    if (!g_started) {
        // 4 buffers × 256 words = 1024 samples. ~42ms of latency at 24kHz;
        // small enough that game→audio lag stays imperceptible, large enough
        // that we survive a few missed refills.
        g_audio.setBuffers(4, 256);
        g_audio.begin(sample_rate);
        pinMode(AMP_EN_PIN, OUTPUT);
        digitalWrite(AMP_EN_PIN, LOW);    // amp OFF until game starts
        g_started = true;
    } else {
        // Sample-rate switch mid-session (game change).
        g_audio.end();
        g_audio.begin(sample_rate);
    }
    g_sample_rate = sample_rate;
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
        int32_t s        = centered * (int32_t)g_volume / 16;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        if (g_audio.availableForWrite() < 2) break;
        g_audio.write((int16_t)s);
        written++;
    }
    return written * 2;   // bytes actually written
}

extern "C" void pico_audio_set_volume(uint8_t vol) {
    if (vol > 16) vol = 16;
    g_volume = vol;
}

extern "C" uint8_t pico_audio_get_volume(void) {
    return g_volume;
}

extern "C" void pico_audio_amp_on(void) {
    digitalWrite(AMP_EN_PIN, HIGH);
}
extern "C" void pico_audio_amp_off(void) {
    digitalWrite(AMP_EN_PIN, LOW);
}
