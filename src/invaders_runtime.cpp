/* invaders_runtime.cpp — runtime-loaded Space Invaders ROM + loader. */

#include <Arduino.h>
#include "pico_romfs.h"   /* ROM zips: SD card or the flash ROM drive */
#include <stdint.h>

extern "C" {
    unsigned char *invaders_rom = nullptr;   /* 0x0000-0x1FFF */
}

extern "C" bool zip_extract_public(const uint8_t *zip, size_t zip_size,
                                   const char *want_name, uint8_t *dst,
                                   size_t dst_capacity, size_t *out_size);

extern "C" bool runtime_load_invaders(void) {
    const char *path = "/roms/invaders.zip";
    uint8_t *zip = nullptr; size_t zip_sz = 0;
    if (!romfs_slurp(path, &zip, &zip_sz)) { Serial.println("[invaders] can't read invaders.zip"); return false; }

    /* MAME order: h @ 0x0000, g @ 0x0800, f @ 0x1000, e @ 0x1800 */
    static const char *names[4] = { "invaders.h", "invaders.g", "invaders.f", "invaders.e" };
    unsigned char *rom = (unsigned char *)malloc(0x2000);
    bool ok = rom != nullptr;
    for (int i = 0; ok && i < 4; i++) {
        size_t got = 0;
        ok = zip_extract_public(zip, zip_sz, names[i], rom + i * 0x800, 0x800, &got) && got == 0x800;
        if (!ok) Serial.printf("[invaders] %s missing or wrong size\n", names[i]);
    }
    free(zip);
    if (!ok) { free(rom); return false; }
    invaders_rom = rom;
    Serial.println("[invaders] runtime load OK");
    return true;
}
