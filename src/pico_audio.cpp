/* pico_audio.cpp — PWM audio implementation. */
/* (PICO_FRUITJAM: I2S to the Fruit Jam's TLV320DAC3100 instead - the first
 * block below, taken from Pico2Atari8's pico_audio.cpp (same lineage, same
 * API); the PWM code is the #else, unchanged.) */

#include <Arduino.h>
#ifdef PICO_FRUITJAM
/* Through a macro: PlatformIO's library finder doesn't see #ifdefs and would
 * build the I2S library into every board — and with USE_TINYUSB every
 * library object is linked whole, which changed the other boards' firmware.
 * The fruitjam env lists I2S in its lib_deps instead. */
#define PICO_I2S_HEADER <I2S.h>
#include PICO_I2S_HEADER
#include <hardware/clocks.h>
#include <PicoTlv320.hpp>
#else
#include <PWMAudio.h>
#endif
#include "pico_audio.h"
#include "pico_board.h"

#ifdef PICO_FRUITJAM
/* ---- I2S + TLV320DAC3100 (Adafruit Fruit Jam) ----------------------------
 *
 * arduino-pico's I2S class (the same AudioBufferManager ring as PWMAudio,
 * DMA_IRQ_0) sends 16-bit stereo frames - the mono sample in both halves -
 * at the emulator's rate: BCLK = 32 x rate (22050 Hz: 705.6 kHz). The codec
 * has no MCLK; its PLL runs from BCLK, so the codec is configured once BCLK
 * runs. Its driver is PicoMultiTrackAudio's PicoTlv320 (only that: the
 * Impl header below, none of the mixer), on the SDK's I2C, not Wire.
 *
 * At 200 MHz no PIO divider is exact at 22050 Hz (22050 has factors 3 and
 * 7), so BCLK has fractional jitter; -DA8_SAMPLE_RATE=25000 gives an exact
 * divider (125) to compare on the bench.
 *
 * Volume stays in the sample path (the same curve as PWM); the codec sits
 * at fixed levels. Amp on / off = the DAC's soft mute (no pop). Output:
 * headphones while some are plugged in, else the speaker (A8_TLV_OUTPUT
 * forces 1 headphones, 2 speaker, 3 both; serial keys, main.cpp). */
#include <PicoTlv320Impl.hpp>          /* the driver's code: in this one file */
#ifdef A8_HDMI_AUDIO
/* sound over HDMI: Pico2Arcade's video.cpp (Pico2Atari8: a8_display.cpp / .h) */
bool a8_display_hdmi_audio(void);        /* the HDMI output carries sound */
uint32_t a8_display_hdmi_audio_gaps(void);
#endif

#ifndef A8_I2S_BCLK_PIN
#define A8_I2S_BCLK_PIN 26             /* WS = 27 (BCLK + 1) */
#endif
#ifndef A8_I2S_DATA_PIN
#define A8_I2S_DATA_PIN 24             /* DIN to the codec */
#endif
#ifndef A8_TLV_SDA_PIN
#define A8_TLV_SDA_PIN 20              /* I2C0, shared with STEMMA QT and HDMI DDC */
#endif
#ifndef A8_TLV_SCL_PIN
#define A8_TLV_SCL_PIN 21
#endif
#ifndef A8_TLV_RESET_PIN
#define A8_TLV_RESET_PIN 22            /* also the ESP32-C6's EN: resets it too */
#endif
#ifndef A8_TLV_OUTPUT
#define A8_TLV_OUTPUT 0                /* 0 auto (headset detect), 1 HP, 2 speaker, 3 both */
#endif

static I2S g_i2s(OUTPUT);
static PicoTlv320 g_codec;
static bool     g_codec_ok = false;
static bool     g_started    = false;
static int      g_sample_rate = 0;
static int      g_in_rate = 0;         /* Pico2Arcade: the game's rate (pico_audio_init) */
static int      g_up = 1;              /*   each of its samples is sent this many times */
static bool     g_amp_on = false;      /* unmuted (wanted); kept across suspend */
static int      g_output_mode = A8_TLV_OUTPUT;
// Volume 0..100 %, applied as gain = pct² / 10000 — the same perceptual
// curve PicoCartReader uses, so a given % sounds the same in both.
static volatile int g_volume_pct = 50;    /* default 50 %: full is too loud */
static volatile int g_gain       = 10000;   // pct², 0..10000

/* Health, reported every 5 s (pico_audio_report). */
static volatile uint32_t g_underruns = 0;      /* pushes that found the ring had run dry */
static volatile uint32_t g_stretched = 0;      /* samples played twice (ring under half) */
static volatile int      g_min_queued = 1 << 30;

#ifdef A8_HDMI_AUDIO
/* Sound over HDMI instead (serial key O, or A8_SOUND_HDMI from start-up):
 * the same samples go into this ring instead of the I2S one, and core 1
 * (a8_display.cpp) takes them two at a time into HDMI audio packets at
 * 44100 Hz. The emulator is paced by this ring as by the I2S one. I2S is
 * stopped and the codec muted meanwhile: two outputs would run off two
 * clocks (the crystal, the HDMI pixel clock) and drift apart. */
static const int HDMI_RING = 512;                      /* 23 ms; the packet queue adds 18 */
static int16_t   g_hring[HDMI_RING];
static volatile uint32_t g_hhead = 0, g_htail = 0;     /* core 0 writes, core 1 reads */
static volatile bool g_hdmi = false;
/* Before pico_audio_init: where the sound is to go (Settings > Sound
 * output); the HDMI output carries sound packets all along (a8_display.cpp),
 * so switching never restarts it. */
#ifndef A8_SOUND_HDMI
#define A8_SOUND_HDMI 0                  /* 1: sound over HDMI from start-up */
#endif
static bool g_hdmi_wanted = A8_SOUND_HDMI;
#endif

static void i2s_start(int rate) {
    g_i2s.setBCLK(A8_I2S_BCLK_PIN);
    g_i2s.setDATA(A8_I2S_DATA_PIN);
    g_i2s.setBitsPerSample(16);
    // 8 buffers × 128 words = 1024 samples, as PWMAudio (see below).
    g_i2s.setBuffers(8, 128);
    bool ok = g_i2s.begin(rate);
    uint32_t sys = clock_get_hz(clk_sys), bclk = (uint32_t)rate * 32;
    Serial.printf("[audio] I2S %s: %d Hz, BCLK %lu Hz (GPIO %d, WS %d, DIN %d), "
                  "PIO divider %.4f (sys %lu MHz)\n",
                  ok ? "running" : "FAILED", rate, (unsigned long)bclk, A8_I2S_BCLK_PIN,
                  A8_I2S_BCLK_PIN + 1, A8_I2S_DATA_PIN, (double)sys / (2.0 * bclk),
                  (unsigned long)(sys / 1000000));
}

static PicoTlv320::Output wanted_output(void) {
    if (g_output_mode) return (PicoTlv320::Output)g_output_mode;
    PicoTlv320::Headset h = g_codec.headset();
    return (h == PicoTlv320::HEADSET_NONE || h == PicoTlv320::HEADSET_UNKNOWN)
           ? PicoTlv320::OUTPUT_SPEAKER : PicoTlv320::OUTPUT_HEADPHONE;
}

static const char *output_name(int o) {
    static const char *const n[4] = { "none", "headphones", "speaker", "speaker + headphones" };
    return n[o & 3];
}

extern "C" void pico_audio_init(int sample_rate) {
    /* Pico2Arcade: below 16 kHz (Donkey Kong's 11765 Hz) each sample goes out
     * twice, at twice the rate: at 11765 Hz BCLK (32 x rate = 376 kHz) is
     * under the codec PLL's 512 kHz minimum input, so there is no clock
     * solution. From here on sample_rate / g_sample_rate are the output's. */
    if (g_started && sample_rate == g_in_rate) return;
    g_in_rate = sample_rate;
    g_up = sample_rate < 16000 ? 2 : 1;
    sample_rate *= g_up;
    if (g_started && sample_rate == g_sample_rate) return;
    if (g_started) {                    /* rate change: the codec's clocks too */
        g_codec.powerDown();
        g_i2s.end();
    } else {
        PicoTlv320::busBegin(i2c0, A8_TLV_SDA_PIN, A8_TLV_SCL_PIN);
        g_codec_ok = g_codec.begin(i2c0, PicoTlv320::DEFAULT_ADDRESS, A8_TLV_RESET_PIN);
        Serial.printf("[audio] TLV320DAC3100 at 0x%02x: %s\n", PicoTlv320::DEFAULT_ADDRESS,
                      g_codec_ok ? "found" : "NOT FOUND");
    }
    i2s_start(sample_rate);             /* BCLK first: the codec's PLL locks to it */
    g_sample_rate = sample_rate;
    g_started = true;
    if (!g_codec_ok) return;
    if (!g_codec.configure((uint32_t)sample_rate * 32, (uint32_t)sample_rate)) {
        Serial.println("[audio] codec configure FAILED (no clock solution?)");
        return;
    }
    const PicoTlv320Clocks &c = g_codec.clocks();
    Serial.printf("[audio] codec PLL P %u R %u J %u = %lu Hz, NDAC %u MDAC %u DOSR %u\n",
                  c.p, c.r, c.j, (unsigned long)c.pllHz, c.ndac, c.mdac, c.dosr);
    g_codec.enableHeadsetDetect(true);
    PicoTlv320::Output o = wanted_output();
    g_codec.setOutput(o);
    g_codec.setMute(!g_amp_on);
    Serial.printf("[audio] output: %s (%s)\n", output_name(o), g_output_mode ? "fixed" : "auto");
#ifdef A8_HDMI_AUDIO
    if (g_hdmi_wanted && a8_display_hdmi_audio()) {   /* HDMI has the sound from the start */
        g_codec.setMute(true);
        g_i2s.end();
        g_hhead = g_htail = 0;
        g_hdmi = true;
        Serial.printf("[audio] sound: HDMI (48000 Hz)%c", 10);
    }
#endif
}

/* Flash writes: I2S off (its DMA can't ride out interrupts off, as PWM's),
 * the codec muted first so BCLK stopping isn't heard; it keeps its setup,
 * and its PLL locks again once BCLK is back. */
extern "C" void pico_audio_suspend(void) {
    if (!g_started) return;
#ifdef A8_HDMI_AUDIO
    if (g_hdmi) return;                  /* I2S is stopped (HDMI has the sound) */
#endif
    if (g_codec.ready()) {
        g_codec.setMute(true);
        delay(10);                       /* soft-step mute: up to ~128 samples (6 ms) */
    }
    g_i2s.end();
}
extern "C" void pico_audio_resume(void) {
    if (!g_started) return;
#ifdef A8_HDMI_AUDIO
    if (g_hdmi) return;
#endif
    i2s_start(g_sample_rate);
    if (g_codec.ready() && g_amp_on) {
        delay(10);                       /* PLL lock (typically 10 ms) */
        g_codec.setMute(false);
    }
}

extern "C" void pico_audio_set_sample_rate(int sample_rate) {
    pico_audio_init(sample_rate);
}

/* As the PWM version below (the stretch while the ring is under half full,
 * and the DC blocker - see there), but each sample goes out as one stereo
 * word (it in both halves), written a block at a time. In RAM: it runs for
 * every sample, and from flash its loop would compete with the emulator's
 * per-scanline code for the XIP cache (config.h, A8_RAM). */
static const int RING_SAMPLES = 8 * 128;               /* setBuffers(8, 128): a word = a frame = a sample */
static int32_t  g_dc_x1 = 0, g_dc_y1 = 0;              /* DC blocker: last in, last out (x256) */
static uint32_t g_stretch_acc = 0;                     /* 16.16: due extra samples */

#ifdef A8_HDMI_AUDIO
/* The same as below, into the HDMI ring (amp off: silence). */
static size_t __not_in_flash_func(push_hdmi)(const int16_t *samples, size_t nsamp) {
    size_t written = 0;
    uint32_t head = g_hhead;
    int queued = (int)(head - g_htail);
    int room = HDMI_RING - queued;
    if (queued < g_min_queued) g_min_queued = queued;
    uint32_t step = queued < HDMI_RING / 2
        ? (uint32_t)(HDMI_RING / 2 - queued) * (65536 / 2) / (HDMI_RING / 2)
        : 0;
    const int32_t gain = g_amp_on ? g_gain : 0;
    while (written < nsamp && room > 0) {
        int32_t x = samples[written];
        g_dc_y1 += ((x - g_dc_x1) << 8) - (g_dc_y1 >> 10);   /* y = x - x1 + (1 - 1/1024) y1 */
        g_dc_x1 = x;
        int32_t s = (g_dc_y1 >> 8) * gain / 10000;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        g_hring[head++ % HDMI_RING] = (int16_t)s;
        room--;
        written++;
        g_stretch_acc += step;
        if (g_stretch_acc >= 65536) {
            g_stretch_acc -= 65536;
            if (room > 0) { g_hring[head++ % HDMI_RING] = (int16_t)s; room--; g_stretched++; }
        }
    }
    __dmb();                             /* the samples before the index (core 1 reads) */
    g_hhead = head;
    return written * 2;
}

/* Core 1 (a8_display.cpp): up to n samples out of the HDMI ring. */
extern "C" size_t __not_in_flash_func(pico_audio_hdmi_take)(int16_t *dst, size_t n) {
    uint32_t tail = g_htail, avail = g_hhead - tail;
    __dmb();
    if (avail < n) return 0;
    for (size_t i = 0; i < n; i++) dst[i] = g_hring[tail++ % HDMI_RING];
    g_htail = tail;
    return n;
}

/* Sound to HDMI or back to the Fruit Jam's codec. */
extern "C" void pico_audio_set_hdmi(int on_) {
    const bool on = on_ != 0;
    if (on == g_hdmi || !g_started) return;
    if (on) {
        if (!a8_display_hdmi_audio()) {  /* (the output carries sound: nothing to restart) */
            Serial.printf("[audio] HDMI sound: not in this HDMI mode%c", 10);
            return;
        }
        if (g_codec.ready()) { g_codec.setMute(true); delay(10); }
        g_i2s.end();
        g_hhead = g_htail = 0;
        g_hdmi = true;
    } else {
        g_hdmi = false;                  /* nothing more into the ring: HDMI sends silence */
        i2s_start(g_sample_rate);
        if (g_codec.ready() && g_amp_on) { delay(10); g_codec.setMute(false); }
    }
    g_min_queued = 1 << 30;
    g_stretched = 0;
    Serial.printf("[audio] sound: %s%c", g_hdmi ? "HDMI (48000 Hz)" : "Fruit Jam", 10);
}
extern "C" void pico_audio_toggle_hdmi(void) { pico_audio_set_hdmi(!g_hdmi); }
/* Pico2Arcade (video.cpp's feed): the HDMI ring's rate (the game's) and fill. */
extern "C" int pico_audio_hdmi_rate(void) { return g_in_rate; }
extern "C" size_t pico_audio_hdmi_avail(void) { return g_hhead - g_htail; }
#endif

/* Galagino's sound (pico_audio_push): blocks of UNSIGNED 16-bit samples
 * centred on 0x8000 (galagino.cpp snd_render_buffer), at 24000 Hz or 11765 Hz
 * (Donkey Kong) — taken whole or not at all: snd_transmit renders the next
 * block once one is taken, and its do..while(bytesOut) stops at the first 0,
 * which is the pacing. (Pico2Atari8's push_s16_raw, with its stretch for an
 * emulator paced by the ring, isn't needed: the game here is paced by the
 * render loop's clock, and its sound by the ring.) The DC blocker and the
 * volume as there. Each sample goes out g_up times (2 below 16 kHz: see
 * pico_audio_init). In RAM, as there. */
/* Diagnosis (serial keys q / Q, pico_audio_key): the game's sound not fed
 * (q: snd_transmit then renders nothing), or I2S stopped as well (Q). */
static volatile bool g_diag_no_feed = false, g_diag_i2s_off = false;

extern "C" size_t __not_in_flash_func(pico_audio_push)(const void *data, size_t bytes) {
    if (!g_started || bytes < 2 || g_diag_no_feed || g_diag_i2s_off) return 0;
#ifdef A8_HDMI_AUDIO
    if (g_hdmi) {
        /* Sound over HDMI: into the HDMI ring (push_hdmi: the DC blocker and
         * the volume as for I2S), whole blocks only, at the game's rate;
         * video.cpp's feed (core 0, the HDMI core here) resamples it to
         * 48 kHz packets. (Was: TODO - Galagino's rates aren't 22050.) */
        const uint16_t *u = (const uint16_t *)data;
        size_t n = bytes / 2;
        if (n > 128) n = 128;
        /* (Not push_hdmi: its stretch - a sample played twice now and then
         * while the ring is under half full, for atari800, which the ring
         * paces - fired all the time here, where the feed keeps the ring
         * about half full and the game's sound is pulled by it: a wavering
         * pitch, "shadow tones" in Galaga on the board, 2026-11-11.) */
        uint32_t head = g_hhead;
        const int queued = (int)(head - g_htail);
        if (HDMI_RING - queued < (int)n) return 0;
        if (queued < g_min_queued) g_min_queued = queued;
        const int32_t gain = g_amp_on ? g_gain : 0;
        for (size_t i = 0; i < n; i++) {
            const int32_t x = (int32_t)u[i] - 32768;
            g_dc_y1 += ((x - g_dc_x1) << 8) - (g_dc_y1 >> 10);   /* y = x - x1 + (1 - 1/1024) y1 */
            g_dc_x1 = x;
            int32_t s = (g_dc_y1 >> 8) * gain / 10000;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            g_hring[head++ % HDMI_RING] = (int16_t)s;
        }
        __dmb();                             /* the samples before the index (the feed reads) */
        g_hhead = head;
        return n * 2;
    }
#endif
    if (g_i2s.getUnderflow()) g_underruns++;           /* the DMA found no buffer: silence played */
    const uint16_t *samples = (const uint16_t *)data;
    size_t nsamp = bytes / 2;
    if (nsamp > 128) nsamp = 128;                      /* (Galagino's blocks are 64) */
    const int up = g_up;
    int room = g_i2s.availableForWrite() / 4;
    int queued = RING_SAMPLES - room;
    if (queued < g_min_queued) g_min_queued = queued;
    if (room < (int)nsamp * up) return 0;              /* all or nothing */
    uint32_t block[128 * 2];
    int nb = 0;
    for (size_t i = 0; i < nsamp; i++) {
        int32_t x = (int32_t)samples[i] - 32768;        /* now -32768..32767 */
        g_dc_y1 += ((x - g_dc_x1) << 8) - (g_dc_y1 >> 10);   /* y = x - x1 + (1 - 1/1024) y1 */
        g_dc_x1 = x;
        int32_t s = (g_dc_y1 >> 8) * g_gain / 10000;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        uint32_t w = (uint16_t)s | ((uint32_t)(uint16_t)s << 16);
        block[nb++] = w;
        if (up > 1) block[nb++] = w;
    }
    g_i2s.write((const uint8_t *)block, nb * 4);
    return nsamp * 2;
}

extern "C" void pico_audio_amp_on(void) {
    g_amp_on = true;
    /* the health counts start here: before it (the machine starting, a
     * pause) the ring ran dry on purpose */
    g_i2s.getUnderflow();
    g_underruns = 0;
    g_stretched = 0;
    g_min_queued = 1 << 30;
    if (g_codec.ready()) g_codec.setMute(false);
}
extern "C" void pico_audio_amp_off(void) {
    g_amp_on = false;
    if (g_codec.ready()) g_codec.setMute(true);
}

/* Main loop: follow the headphone jack (I2C, so not more than 4 times a
 * second; about 0.1 ms each). */
extern "C" void pico_audio_poll(void) {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (!g_codec.ready() || g_output_mode || now - last < 250) return;
#ifdef A8_HDMI_AUDIO
    if (g_hdmi) return;                  /* the codec is muted meanwhile */
#endif
    last = now;
    PicoTlv320::Headset h = g_codec.headset();
    if (h == PicoTlv320::HEADSET_UNKNOWN) return;
    PicoTlv320::Output o = h == PicoTlv320::HEADSET_NONE ? PicoTlv320::OUTPUT_SPEAKER
                                                         : PicoTlv320::OUTPUT_HEADPHONE;
    if (o != g_codec.output()) {
        g_codec.setOutput(o);
        Serial.printf("[audio] output -> %s\n", output_name(o));
    }
}

extern "C" void pico_audio_set_output(int mode) {
    g_output_mode = mode & 3;
    if (!g_codec.ready()) return;
    PicoTlv320::Output o = wanted_output();
    g_codec.setOutput(o);
    Serial.printf("[audio] output: %s (%s)\n", output_name(o), g_output_mode ? "fixed" : "auto");
}

/* Settings > Sound output (a8_settings.cpp): 0 - 3 the codec's outputs as
 * pico_audio_set_output, 4 HDMI. Before pico_audio_init (the menu, no
 * sound yet) it is kept for it. */
extern "C" void pico_audio_set_sound_output(int o) {
#ifdef A8_HDMI_AUDIO
    if (o < 0 || o > 4) o = 0;
#else
    if (o < 0 || o > 3) o = 0;
#endif
    if (o < 4 && o != g_output_mode) pico_audio_set_output(o);
#ifdef A8_HDMI_AUDIO
    if (g_started) pico_audio_set_hdmi(o == 4);
    else if (o != 4 || a8_display_hdmi_audio()) g_hdmi_wanted = o == 4;   /* (no room: stays) */
#endif
}
extern "C" int pico_audio_get_sound_output(void) {
#ifdef A8_HDMI_AUDIO
    if (g_started ? g_hdmi : g_hdmi_wanted) return 4;
#endif
    return g_output_mode;
}


/* The serial monitor's sound keys (a8_input.cpp passes on the keys it
 * doesn't use): + / - volume, A H S B output (auto, headphones, speaker,
 * both), I the report now. */
extern "C" void pico_audio_key(int c) {
    switch (c) {
    case '+': case '=': pico_audio_set_volume_pct(g_volume_pct + 10); break;
    case '-': case '_': pico_audio_set_volume_pct(g_volume_pct - 10); break;
    case 'A': pico_audio_set_output(0); return;
    case 'H': pico_audio_set_output(1); return;
    case 'S': pico_audio_set_output(2); return;
    case 'B': pico_audio_set_output(3); return;
#ifdef A8_HDMI_AUDIO
    case 'o': case 'O': pico_audio_toggle_hdmi(); return;   /* Fruit Jam codec <-> HDMI (not saved) */
#endif
    case 'I': pico_audio_report(); return;
    case 'q':                            /* diagnosis: stop / resume feeding the sound */
        g_diag_no_feed = !g_diag_no_feed;
        Serial.printf("[audio] DIAG feed %s%c", g_diag_no_feed ? "OFF (no sound rendered)" : "on", 10);
        return;
    case 'Q':                            /* diagnosis: I2S itself off / on */
        g_diag_i2s_off = !g_diag_i2s_off;
        if (g_diag_i2s_off) pico_audio_suspend(); else pico_audio_resume();
        Serial.printf("[audio] DIAG I2S %s%c", g_diag_i2s_off ? "STOPPED" : "running", 10);
        return;
    default: return;
    }
    Serial.printf("[audio] volume %d %%%c", g_volume_pct, 10);
}

/* Every 5 s from main.cpp: what the ring went through since the last one. */
extern "C" void pico_audio_report(void) {
    int minq = g_min_queued;
    uint32_t under = g_underruns, str = g_stretched;
    g_min_queued = 1 << 30;
    g_underruns = 0;
    g_stretched = 0;
#ifdef A8_HDMI_AUDIO
    if (g_hdmi) {
        static uint32_t last_gaps = 0;
        uint32_t gaps = a8_display_hdmi_audio_gaps();
        Serial.printf("[audio] HDMI 48000 Hz, ring low %d of %d samples, %lu stretched, "
                      "%lu gaps (silence packets), volume %d %%%s%c",
                      minq > HDMI_RING ? -1 : minq, HDMI_RING, (unsigned long)str,
                      (unsigned long)(gaps - last_gaps), g_volume_pct, g_amp_on ? "" : " (muted)", 10);
        last_gaps = gaps;
        return;
    }
#endif
    Serial.printf("[audio] %d Hz, ring low %d of %d samples, %lu stretched, %lu under-runs, "
                  "volume %d %%, %s%s\n",
                  g_sample_rate, minq > RING_SAMPLES ? -1 : minq, RING_SAMPLES,
                  (unsigned long)str, (unsigned long)under, g_volume_pct,
                  output_name(g_codec.output()), g_amp_on ? "" : " (muted)");
}
#else   /* PWM */

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
static volatile int g_volume_pct = 50;    /* default 50 %: full is too loud */
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

#endif  /* PWM */

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

#ifndef PICO_FRUITJAM
extern "C" void pico_audio_amp_on(void) {
    if (AMP_EN_PIN >= 0) digitalWrite(AMP_EN_PIN, HIGH);
}
extern "C" void pico_audio_amp_off(void) {
    if (AMP_EN_PIN >= 0) digitalWrite(AMP_EN_PIN, LOW);
}
#endif
