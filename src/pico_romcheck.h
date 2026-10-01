/* pico_romcheck.h — is a ROM zip the supported MAME set? */
#ifndef PICO_ROMCHECK_H
#define PICO_ROMCHECK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True if every file the loader needs is in `zipPath` ("/roms/galaga.zip")
 * with the expected CRC32. Otherwise false, with a short reason in `why`
 * (e.g. "2 files missing (gg1_9.4l)"). */
bool romcheck_zip(const char *zipPath, char *why, size_t whyLen);

/* Expected CRC32s of a ROM file name, from the ROM-check tables (a name can be
 * in more than one set, e.g. "5e"). Fills up to `max`, returns the count.
 * Lets the loaders find a file that a zip stores under another name (older
 * or newer MAME versions renamed many ROMs; the contents are the same). */
int romcheck_crcs_for(const char *name, uint32_t *crcs, int max);

#ifdef __cplusplus
}
#endif

#endif
