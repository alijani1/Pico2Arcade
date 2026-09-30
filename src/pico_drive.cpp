/* pico_drive.cpp — see pico_drive.h. */
#include <Arduino.h>
#include "pico_board.h"

#ifdef PICO_USB_DRIVE
#include <Adafruit_TinyUSB.h>
#include <TFT_eSPI.h>
#include <hardware/flash.h>
#include <hardware/sync.h>
#include "pico_drive.h"
#include "pico_romfs.h"
#include "pico_handoff.h"
#include "pico_audio.h"

#ifdef PICO_GALAGINO_LANDSCAPE
extern "C" volatile int g_landscape_mode;
#endif

static const uint32_t SECTOR = 512;
static const uint32_t ROOT_ENTRIES = 64;               /* 4 sectors */

static const uint8_t *s_xip;                           /* drive via XIP (reads) */
static uint32_t s_phys, s_sectors;                     /* physical offset (writes), size */

/* Writes go through one 4 KB flash-sector cache: erase + program happen only
 * when a write moves to another sector, on the host's flush, after a short
 * idle, and on exit — not per 512-byte block. Core 1 is parked in setup1()
 * (running from flash), so it is idled around every flash write. */
static uint8_t  *s_cache;                              /* malloc'd: RAM is tight in game mode */
static int32_t   s_cacheSector = -1;
static bool      s_cacheDirty = false;
static uint32_t  s_lastWriteMs = 0;

static void flush_cache(void) {
    if (!s_cacheDirty || s_cacheSector < 0) return;
    uint32_t ofs = s_phys + (uint32_t)s_cacheSector * FLASH_SECTOR_SIZE;
    rp2040.idleOtherCore();
    uint32_t irq = save_and_disable_interrupts();      /* no XIP code may run meanwhile */
    flash_range_erase(ofs, FLASH_SECTOR_SIZE);
    flash_range_program(ofs, s_cache, FLASH_SECTOR_SIZE);
    restore_interrupts(irq);
    rp2040.resumeOtherCore();
    s_cacheDirty = false;
}

static void load_sector(int32_t sector) {
    if (sector == s_cacheSector) return;
    flush_cache();
    memcpy(s_cache, s_xip + (uint32_t)sector * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
    s_cacheSector = sector;
}

static int32_t drive_read(uint32_t lba, void *buffer, uint32_t bufsize) {
    uint8_t *out = (uint8_t *)buffer;
    for (uint32_t done = 0; done < bufsize; done += SECTOR, lba++) {
        if (lba >= s_sectors) return -1;
        uint32_t byteOfs = lba * SECTOR;
        int32_t sector = byteOfs / FLASH_SECTOR_SIZE;
        const uint8_t *src = (sector == s_cacheSector) ? s_cache + (byteOfs % FLASH_SECTOR_SIZE)
                                                       : s_xip + byteOfs;
        memcpy(out + done, src, SECTOR);
    }
    return (int32_t)bufsize;
}

static int32_t drive_write(uint32_t lba, uint8_t *buffer, uint32_t bufsize) {
    for (uint32_t done = 0; done < bufsize; done += SECTOR, lba++) {
        if (lba >= s_sectors) return -1;
        uint32_t byteOfs = lba * SECTOR;
        load_sector(byteOfs / FLASH_SECTOR_SIZE);
        memcpy(s_cache + (byteOfs % FLASH_SECTOR_SIZE), buffer + done, SECTOR);
        s_cacheDirty = true;
    }
    s_lastWriteMs = millis();
    return (int32_t)bufsize;
}

static void drive_flush(void) { flush_cache(); }

static void write_sector(uint32_t lba, const uint8_t *data) {
    drive_write(lba, (uint8_t *)data, SECTOR);
}

static bool is_formatted(void) {
    return s_xip[510] == 0x55 && s_xip[511] == 0xAA && memcmp(s_xip + 54, "FAT12", 5) == 0;
}

static void fat12_set(uint8_t *fat, uint32_t c, uint32_t v) {
    uint32_t o = c + c / 2;
    if (c & 1) { fat[o] = (fat[o] & 0x0F) | ((v << 4) & 0xF0); fat[o + 1] = (v >> 4) & 0xFF; }
    else       { fat[o] = v & 0xFF; fat[o + 1] = (fat[o + 1] & 0xF0) | ((v >> 8) & 0x0F); }
}

/* FAT12 sized to the region: 512-byte sectors, 1 sector per cluster, 2 FATs,
 * 64 root entries (Quad 256 KB: the same layout the Quad firmware formats).
 * `file`, if given, is written to the root as `name83` (standalone: the
 * default ARCADE.INI). */
static void format_drive(const char label[11], const char name83[11], const char *file) {
    uint32_t spf = ((s_sectors + 2) * 3 / 2 + SECTOR - 1) / SECTOR;    /* sectors per FAT */
    uint32_t rootLba = 1 + 2 * spf, rootSecs = ROOT_ENTRIES * 32 / SECTOR;
    uint32_t dataLba = rootLba + rootSecs;
    uint32_t fileLen = file ? strlen(file) : 0;
    uint32_t fileClusters = (fileLen + SECTOR - 1) / SECTOR;           /* < 340: fits FAT sector 0 */
    uint8_t s[SECTOR];

    /* Boot sector / BPB */
    memset(s, 0, sizeof(s));
    s[0] = 0xEB; s[1] = 0x3C; s[2] = 0x90;
    memcpy(s + 3, "MSDOS5.0", 8);
    s[11] = SECTOR & 0xFF; s[12] = SECTOR >> 8;             /* bytes per sector */
    s[13] = 1;                                               /* sectors per cluster */
    s[14] = 1;                                               /* reserved sectors */
    s[16] = 2;                                               /* FAT count */
    s[17] = ROOT_ENTRIES;                                    /* root directory entries */
    s[19] = s_sectors & 0xFF; s[20] = s_sectors >> 8;        /* total sectors */
    s[21] = 0xF8;                                            /* media descriptor */
    s[22] = spf;                                             /* sectors per FAT */
    s[24] = 32; s[26] = 2;                                   /* sectors/track, heads */
    s[36] = 0x80;                                            /* drive number */
    s[38] = 0x29;                                            /* extended boot signature */
    s[39] = 0x21; s[40] = 0x43; s[41] = 0x65; s[42] = 0x87;  /* volume serial */
    memcpy(s + 43, label, 11);
    memcpy(s + 54, "FAT12   ", 8);
    s[510] = 0x55; s[511] = 0xAA;
    write_sector(0, s);

    /* Two FATs: media byte + end-of-chain for clusters 0/1, then the file's chain */
    for (uint32_t fat = 0; fat < 2; fat++) {
        for (uint32_t i = 0; i < spf; i++) {
            memset(s, 0, sizeof(s));
            if (i == 0) {
                s[0] = 0xF8; s[1] = 0xFF; s[2] = 0xFF;
                for (uint32_t c = 0; c < fileClusters; c++)
                    fat12_set(s, 2 + c, c + 1 < fileClusters ? 3 + c : 0xFFF);
            }
            write_sector(1 + fat * spf + i, s);
        }
    }

    /* Root directory: volume label entry, the file, rest empty */
    memset(s, 0, sizeof(s));
    memcpy(s, label, 11);
    s[11] = 0x08;                                            /* ATTR_VOLUME_ID */
    if (file) {
        uint8_t *e = s + 32;
        memcpy(e, name83, 11);
        e[11] = 0x20;                                        /* ATTR_ARCHIVE */
        e[24] = 0x21; e[25] = 0x5D;                          /* date 2026-09-01 */
        e[26] = 2;                                           /* first cluster */
        e[28] = fileLen & 0xFF; e[29] = (fileLen >> 8) & 0xFF;
        e[30] = (fileLen >> 16) & 0xFF; e[31] = fileLen >> 24;
    }
    write_sector(rootLba, s);
    memset(s, 0, sizeof(s));
    for (uint32_t i = 1; i < rootSecs; i++) write_sector(rootLba + i, s);

    /* File data; the rest of the data area needs no clearing (FATs mark it free). */
    for (uint32_t c = 0; c < fileClusters; c++) {
        memset(s, 0, sizeof(s));
        uint32_t n = fileLen - c * SECTOR;
        memcpy(s, file + c * SECTOR, n > SECTOR ? SECTOR : n);
        write_sector(dataLba + c, s);
    }
    flush_cache();
}

static inline bool pin_low(int p) { return p >= 0 && digitalRead(p) == LOW; }

extern "C" bool pico_drive_requested(void) {
    int f = handoff_flags_read();
    if (f >= 0 && (f & HANDOFF_FLAG_DRIVE)) {
        handoff_flags_write(f & ~HANDOFF_FLAG_DRIVE);       /* exit returns to the list */
        return true;
    }
#ifdef PICO_STANDALONE
    if (pin_low(g_pins.menu)) return true;                  /* held at power-on */
    const uint8_t *xip; uint32_t phys, size;
    romfs_location(&xip, &phys, &size);
    if (!(xip[510] == 0x55 && xip[511] == 0xAA)) return true;   /* first boot: set it up */
#endif
    return false;
}

extern "C" void pico_drive_request(void) {
    int f = handoff_flags_read();
    handoff_flags_write((f < 0 ? 0 : f) | HANDOFF_FLAG_DRIVE);
    handoff_relaunch_arcade();
}

static Adafruit_USBD_MSC s_msc;

extern "C" void pico_drive_run(void) {
    uint32_t size;
    romfs_location(&s_xip, &s_phys, &size);
    s_sectors = size / SECTOR;
    pico_audio_suspend();        /* no audio here; its DMA can't survive flash writes (pico_audio.cpp) */
    s_cache = (uint8_t *)malloc(FLASH_SECTOR_SIZE);
    Serial.printf("[drive] ROM drive mode: 0x%lx, %lu sectors\n",
                  (unsigned long)s_phys, (unsigned long)s_sectors);

    /* One name on every board (the Quad's first drives were "QUAD ROMS"; an
     * already formatted drive keeps its label). */
    const char *driveName = "ARCADE ROMS";
    static const char LABEL[11] = {'A','R','C','A','D','E',' ','R','O','M','S'};
    if (!is_formatted()) {
#ifdef PICO_STANDALONE
        static const char INI83[11] = {'A','R','C','A','D','E',' ',' ','I','N','I'};
        format_drive(LABEL, INI83, board_default_ini());
#else
        format_drive(LABEL, nullptr, nullptr);
#endif
    }

    /* USB: add the drive, then re-enumerate so the host sees it. */
    TinyUSBDevice.setProductDescriptor("Arcade ROM Drive");
    s_msc.setID("Galagino", "ROM Drive", "1.0");
    s_msc.setReadWriteCallback(drive_read, drive_write, drive_flush);
    s_msc.setCapacity(s_sectors, SECTOR);
    s_msc.setUnitReady(true);
    s_msc.begin();
    TinyUSBDevice.detach();
    delay(20);
    TinyUSBDevice.attach();

    TFT_eSPI tft;
#ifdef PICO_GALAGINO_LANDSCAPE
    tft.setRotation(g_landscape_mode ? 3 : 0);
#endif
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(10, 20);
    tft.print("ROM Drive (USB)");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    int y = 56;
    tft.setCursor(10, y);      tft.printf("Copy the MAME ROM zips to the");
    tft.setCursor(10, y += 12); tft.printf("%s drive (root or ROMS folder).", driveName);
#ifdef PICO_STANDALONE
    tft.setCursor(10, y += 20); tft.print("Pins: edit ARCADE.INI on the drive.");
#endif
    tft.setCursor(10, y += 20); tft.print("Zips: puckman mspacman galaga");
    tft.setCursor(10, y += 12); tft.print("      frogger dkong digdug");
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(10, y += 24); tft.print("Eject the drive, then press");
    tft.setCursor(10, y += 12); tft.print("MENU or START to play.");

    delay(300);
    while (pin_low(g_pins.menu) || pin_low(g_pins.start)) delay(10);   /* released from entry */

    for (;;) {
        /* Commit a partially written sector once the host goes quiet. */
        if (s_cacheDirty && millis() - s_lastWriteMs > 500) flush_cache();
        if (pin_low(g_pins.menu) || pin_low(g_pins.start)) break;
        delay(5);
    }

    flush_cache();
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 100);
    tft.print("Restarting...");
    delay(300);
    handoff_relaunch_arcade();
    for (;;) {}
}

#endif  /* PICO_USB_DRIVE */
