/* pico_handoff.cpp — see pico_handoff.h. */
#include <Arduino.h>
#include <hardware/structs/watchdog.h>
#include "pico_handoff.h"

extern "C" int handoff_volume_read(void) {
    uint32_t v = watchdog_hw->scratch[HANDOFF_SCRATCH_VOLUME];
    if ((v & 0xFFFF0000u) != HANDOFF_VOLUME_TAG) return -1;
    int pct = (int)(v & 0xFF);
    return pct <= 100 ? pct : -1;
}

/* Option flags live in bits 8..15 of the volume register; keep them when the
 * volume changes and vice versa. */
static uint32_t handoff_flag_bits(void) {
    uint32_t v = watchdog_hw->scratch[HANDOFF_SCRATCH_VOLUME];
    return ((v & 0xFFFF0000u) == HANDOFF_VOLUME_TAG) ? (v & 0xFF00u) : 0;
}

extern "C" void handoff_volume_write(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    watchdog_hw->scratch[HANDOFF_SCRATCH_VOLUME] =
        HANDOFF_VOLUME_TAG | handoff_flag_bits() | (uint32_t)pct;
}

extern "C" int handoff_flags_read(void) {
    uint32_t v = watchdog_hw->scratch[HANDOFF_SCRATCH_VOLUME];
    if ((v & 0xFFFF0000u) != HANDOFF_VOLUME_TAG) return -1;
    return (int)((v >> 8) & 0xFF);
}

extern "C" void handoff_flags_write(int flags) {
    uint32_t v = watchdog_hw->scratch[HANDOFF_SCRATCH_VOLUME];
    uint32_t pct = ((v & 0xFFFF0000u) == HANDOFF_VOLUME_TAG) ? (v & 0xFFu) : 100;
    watchdog_hw->scratch[HANDOFF_SCRATCH_VOLUME] =
        HANDOFF_VOLUME_TAG | ((uint32_t)(flags & 0xFF) << 8) | pct;
}

extern "C" void handoff_relaunch_arcade(void) {
    Serial.println("[handoff] relaunch arcade");
    watchdog_hw->scratch[HANDOFF_SCRATCH_ARCADE] = HANDOFF_ARCADE_MAGIC;
    delay(50);
    rp2040.reboot();
}

extern "C" void handoff_exit_to_cartreader(void) {
    Serial.println("[handoff] exit to CartReader");
    watchdog_hw->scratch[HANDOFF_SCRATCH_ARCADE] = 0;
    delay(50);
    rp2040.reboot();
}
