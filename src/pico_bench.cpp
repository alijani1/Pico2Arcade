/* pico_bench.cpp — see pico_bench.h. Compiled to nothing unless Z80_BENCH. */
#ifdef Z80_BENCH
#include <Arduino.h>
#include <SD.h>
#include <stdarg.h>
#include <hardware/structs/watchdog.h>
#include "pico_bench.h"
#include "pico_handoff.h"

#ifdef Z80_CORE_ZAZU
static const char *CORE = "zazu";
#else
static const char *CORE = "marat";
#endif

extern "C" unsigned char *memory;   /* game RAM, emulation.c */
static const unsigned BENCH_HASH_BYTES = 8192;
static const unsigned long FRAME_BUDGET_US = 16667;

#define BENCH_SCRATCH 2
#define BENCH_TAG     0x42450000u   /* 'BE' | index */

static const char *s_label = "?";
static int  s_index = 0;
static bool s_active = false;
static unsigned long s_frames = 0, s_min = ~0ul, s_max = 0, s_over = 0;
static unsigned long long s_sum = 0;

/* Results go to serial AND to /bench_<core>.log on the SD card (USB
 * serial isn't always available when launched from the CartReader).
 * Lines are buffered and written once per game, so SD access never
 * lands inside the measured frames. */
static char s_buf[4096];
static size_t s_len = 0;

static void blog(const char *fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
    Serial.print(line);
    if (s_len + n < sizeof(s_buf)) { memcpy(s_buf + s_len, line, n); s_len += n; }
}

static void flush_to_sd(bool truncate) {
    char path[32];
    snprintf(path, sizeof(path), "/bench_%s.log", CORE);
    if (truncate) SD.remove(path);
    File f = SD.open(path, FILE_WRITE);   /* FILE_WRITE appends */
    if (f) { f.write((const uint8_t *)s_buf, s_len); f.close(); }
    else   Serial.printf("[BENCH] cannot write %s\n", path);
    s_len = 0;
}

static uint32_t ram_hash(void) {     /* FNV-1a */
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < BENCH_HASH_BYTES; i++) { h ^= memory[i]; h *= 16777619u; }
    return h;
}

extern "C" int bench_game_index(void) {
    uint32_t v = watchdog_hw->scratch[BENCH_SCRATCH];
    return ((v & 0xFFFF0000u) == BENCH_TAG) ? (int)(v & 0xFFFF) : 0;
}

extern "C" void bench_begin(const char *label, int index) {
    s_label = label; s_index = index; s_active = true;
    blog("[BENCH] start core=%s game=%s warmup=%d frames=%d\n",
         CORE, label, BENCH_WARMUP_FRAMES, BENCH_FRAMES);
}

extern "C" void bench_frame(unsigned long us) {
    if (!s_active) return;
    s_frames++;                          /* counts warm-up frames too */
    if (s_frames > BENCH_WARMUP_FRAMES) {
        s_sum += us;
        if (us < s_min) s_min = us;
        if (us > s_max) s_max = us;
        if (us > FRAME_BUDGET_US) s_over++;
    }
    if (s_frames % 60 == 0)
        blog("[BENCH] core=%s game=%s frame=%lu hash=%08lx\n",
             CORE, s_label, s_frames, (unsigned long)ram_hash());
    if (s_frames < BENCH_WARMUP_FRAMES + BENCH_FRAMES) return;

    s_active = false;
    unsigned long measured = s_frames - BENCH_WARMUP_FRAMES;
    blog("[BENCH] RESULT core=%s game=%s measured_frames=%lu (after %d warm-up) "
         "avg_us=%lu min_us=%lu max_us=%lu over_budget=%lu final_hash=%08lx\n",
         CORE, s_label, measured, BENCH_WARMUP_FRAMES,
         (unsigned long)(s_sum / measured),
         s_min, s_max, s_over, (unsigned long)ram_hash());
    flush_to_sd(s_index == 0);          /* first game starts a fresh file */
    watchdog_hw->scratch[BENCH_SCRATCH] = BENCH_TAG | (uint32_t)(s_index + 1);
    Serial.flush();
    handoff_relaunch_arcade();          /* reboot into the next game */
}

extern "C" void bench_all_done(void) {
    watchdog_hw->scratch[BENCH_SCRATCH] = 0;   /* next power-up starts over */
    blog("[BENCH] DONE core=%s\n", CORE);
    flush_to_sd(false);
    while (true) delay(1000);
}
#endif
