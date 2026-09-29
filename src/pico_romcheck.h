/* pico_romcheck.h — is a ROM zip the supported MAME set? */
#ifndef PICO_ROMCHECK_H
#define PICO_ROMCHECK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True if every file the loader needs is in `zipPath` ("/roms/galaga.zip")
 * with the expected CRC32. Otherwise false, with a short reason in `why`
 * (e.g. "2 files missing (gg1_9.4l)"). */
bool romcheck_zip(const char *zipPath, char *why, size_t whyLen);

#ifdef __cplusplus
}
#endif

#endif
