/* pico_freertos_stubs.c — cross-core sync for Galagino's vblank pacing.
 *
 * xTaskNotifyGive (called from core 0's render loop, once per frame)
 * increments a shared counter. ulTaskNotifyTake (called from core 1's
 * emulation task) waits for the counter to go non-zero, then decrements
 * it. This throttles the emulator to ~60 Hz instead of free-running at
 * whatever speed the CPU can manage.
 */

#include <stdint.h>
#include <stdlib.h>
#include <Arduino.h>

/* C-linkage bridge to Serial.print(), defined in pico_serial_log.cpp.
 * Needed because plain printf() from a .c file routes to UART, not USB CDC. */
extern void pico_serial_log(const char *fmt, ...);

/* Stub for ESP32's built-in hardware RNG — used by Galagino's attract
 * mode to pick a random next game. rand() is fine here. */
uint32_t esp_random(void) { return (uint32_t)rand(); }

typedef uint32_t TickType_t;

/* Vblank pulse count. Written by render loop on core 0, drained by
 * emulation on core 1. Volatile so both cores see updates. */
static volatile uint32_t g_vblank_pulses = 0;

__attribute__((weak))
void vTaskDelay(TickType_t ticks) {
  delay((unsigned long)ticks);
}

__attribute__((weak))
uint32_t ulTaskNotifyTake(int clearOnExit, TickType_t timeoutTicks) {
  (void)clearOnExit;   /* see note below */
  unsigned long start = millis();
  while (g_vblank_pulses == 0) {
    if (timeoutTicks != 0xFFFFFFFFu &&
        (millis() - start) >= (unsigned long)timeoutTicks) {
      return 0;   /* timed out */
    }
    delay(1);
  }
  /* Always decrement by one, regardless of clearOnExit.
   *
   * Real FreeRTOS with pdTRUE would clear the count to zero and return
   * the old count. Galagino's emulate_frame does ONE frame per call
   * though — so if we clear the count, any second pulse from the render
   * loop's VIDEO_HALF_RATE path (2 pulses per screen refresh) is lost
   * and the emulator runs at half speed. Decrement keeps every pulse
   * accounted for. */
  uint32_t was = g_vblank_pulses;
  g_vblank_pulses--;

  /* Instrumentation: every 60 successful takes, print the emulator's
     effective frame rate so we can compare to render fps. */
  {
    static uint32_t takes = 0;
    static unsigned long lastMs = 0;
    takes++;
    if (takes % 60 == 0) {
      unsigned long now = millis();
      unsigned long dt = now - lastMs;
      lastMs = now;
      pico_serial_log("[emu] 60 takes in %lu ms = %lu fps\n",
                      dt, dt ? 60000ul / dt : 0ul);
    }
  }

  return was;
}

/* Called by xTaskNotifyGive stub (inline in pico_compat.h, C++). */
void __pico_notify_give(void) {
  g_vblank_pulses++;
}
