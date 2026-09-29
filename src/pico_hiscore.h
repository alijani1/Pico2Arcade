/* pico_hiscore.h — keep the games' high-score tables across power cycles.
 *
 * The arcade boards lost their high scores at power-off. Like MAME's hiscore
 * plugin (and using the addresses from its hiscore.dat), we wait until a game
 * has written its default table after boot, overwrite it with the saved one,
 * and save it again whenever it changes and then stays unchanged for a while
 * (i.e. after the initials are entered), or when leaving the game.
 *
 * Storage: standalone Pico 2 — a flash sector above the settings; Quad —
 * the last 4 KB of the arcade slot (just below the ROM drive); PicoCartReader
 * — /gal_hi.bin on the SD card.
 */
#ifndef PICO_HISCORE_H
#define PICO_HISCORE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Load the saved tables (core 0, before the game starts). */
void pico_hiscore_load(void);
/* Core 1, after every emulated frame: restore / watch the table. */
void pico_hiscore_frame(void);
/* Core 0, from loop(): does pending saves (flash / SD) and a pending exit. */
void pico_hiscore_service(void);
/* Leave the game for the game list, saving the table first if it changed.
 * Never returns. */
void pico_hiscore_exit_to_list(void);

#ifdef __cplusplus
}
#endif

#endif
