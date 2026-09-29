/* pico_sd.cpp — SD card init + directory scan.
 *
 * First milestone toward runtime ROM loading: prove the SD stack works
 * in this project and can enumerate /roms/*.zip. Next steps stack on
 * top of this — unzip in RAM, run the converters, hand decoded blobs
 * to the emulator.
 */

#include <Arduino.h>

/* The Quad has no SD card (its ROMs come from the flash ROM drive — see
 * pico_romfs.cpp), and SPI1 pins 10-13 are its DB9 fire buttons there. */
#ifndef PICO_QUAD
#include <SPI.h>
#include <SD.h>

/* SD wiring — matches PicoCartReader (SPI1 dedicated to the SD card,
 * TFT stays on SPI0). Pin numbers come from platformio.ini defines. */
static SPIClassRP2040 spiSD(spi1, SD_MISO_PIN, /*CS*/-1, SD_SCK_PIN, SD_MOSI_PIN);
static bool sd_ready = false;

extern "C" bool pico_sd_init(void) {
    Serial.println("[sd] SD.begin on SPI1 (SCK=10 MISO=12 MOSI=11 CS=13)");
    sd_ready = SD.begin(SD_CS_PIN, spiSD);
    Serial.printf("[sd] SD.begin: %s\n", sd_ready ? "OK" : "not present");
    if (!sd_ready) return false;

    /* Enumerate /roms/*.zip so we can see what games are available. */
    File dir = SD.open("/roms");
    if (!dir) {
        Serial.println("[sd] /roms directory not found — create it and drop ZIPs there");
        return true;
    }
    if (!dir.isDirectory()) {
        Serial.println("[sd] /roms exists but is not a directory");
        dir.close();
        return true;
    }
    Serial.println("[sd] /roms contents:");
    int nzip = 0;
    while (File f = dir.openNextFile()) {
        const char *name = f.name();
        uint32_t sz = f.size();
        bool is_zip = false;
        size_t nlen = strlen(name);
        if (nlen > 4) {
            const char *ext = name + nlen - 4;
            is_zip = (strcasecmp(ext, ".zip") == 0);
        }
        Serial.printf("[sd]   %s  %lu bytes%s\n",
                      name, (unsigned long)sz, is_zip ? "  [ZIP]" : "");
        if (is_zip) nzip++;
        f.close();
    }
    dir.close();
    Serial.printf("[sd] found %d .zip files\n", nzip);
    return true;
}
#endif  /* !PICO_QUAD */
