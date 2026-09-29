/* pico_bench.h — Z80 core A/B benchmark (build with -DZ80_BENCH).
 *
 * Runs every installed game (in menu order) in attract mode for
 * BENCH_FRAMES frames with all input forced off, reporting per-frame
 * CPU-emulation time and a RAM hash every 60 frames over serial, then
 * reboots into the next game (index kept in watchdog scratch[2]).
 * Identical hash sequences between two cores => same emulation. */
#ifndef PICO_BENCH_H
#define PICO_BENCH_H

/* Frames run before measuring starts. Games boot/attract lighter than
 * real play for the first ~30 s, which would flatter the averages.
 * RAM hashes are still printed during warm-up. */
#ifndef BENCH_WARMUP_FRAMES
#define BENCH_WARMUP_FRAMES 1800   /* 30 s at 60 Hz */
#endif

/* Frames measured after warm-up. */
#ifndef BENCH_FRAMES
#define BENCH_FRAMES 1800          /* 30 s at 60 Hz */
#endif

/* Optional: run only this game, matched against its ROM zip name, e.g.
 * -DBENCH_GAME=galaga (no quotes). Unset = first BENCH_MAX_GAMES
 * installed games in menu order. */
#define BENCH_STR_(x) #x
#define BENCH_STR(x)  BENCH_STR_(x)

/* How many installed games to run, in menu order (raise to cover all). */
#ifndef BENCH_MAX_GAMES
#define BENCH_MAX_GAMES 1
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* Index (into the menu's available-games list) to run this boot. */
int  bench_game_index(void);
/* Start measuring a game. */
void bench_begin(const char *label, int index);
/* Record one frame's emulation time; ends the run when done. */
void bench_frame(unsigned long emu_us);
/* Called when the index is past the last game. Never returns. */
void bench_all_done(void);
#ifdef __cplusplus
}
#endif

#endif
