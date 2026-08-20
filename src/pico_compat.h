/* pico_compat.h — Thin ESP32→RP2350 compatibility shim for Galagino.
 *
 * Prepended to galagino.cpp. Provides Arduino-Pico substitutes for
 * ESP32-specific APIs so the upstream sketch compiles.
 *
 * Semantics of stubbed calls:
 *   FreeRTOS task API → serialized (no second core yet; game runs on core 0
 *                       and the "emulation task" is called inline from loop)
 *   ESP.*             → returns 0 / benign default
 *   setCpuFrequencyMhz → no-op (fixed in platformio.ini)
 *
 * Real Pico dual-core wiring can come later — see PicoCartReader main.cpp
 * for a working setup1()/loop1() pattern.
 */

#ifndef PICO_GALAGINO_COMPAT_H
#define PICO_GALAGINO_COMPAT_H

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* --- ESP-IDF version — force the "old enough, no workaround" path --- */
#ifndef ESP_IDF_VERSION_VAL
#define ESP_IDF_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))
#endif
#ifndef ESP_IDF_VERSION
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(4,0,0)
#endif

/* --- FreeRTOS task/queue stubs --- */
typedef void* TaskHandle_t;
typedef uint32_t TickType_t;
#define portTICK_PERIOD_MS 1
#define tskNO_AFFINITY -1

static inline void vTaskDelay(TickType_t t) { delay((unsigned)t); }
static inline uint32_t uxTaskPriorityGet(TaskHandle_t) { return 1; }
/* xTaskCreatePinnedToCore stub — records the pending task so core 1
   (setup1() in earlephilhower framework) can pick it up and run it.
   Galagino's emulation_task never returns; we run it as core 1's body. */
extern void (* volatile g_pending_task_fn)(void*);
extern void *          g_pending_task_arg;
static inline int xTaskCreatePinnedToCore(void (*fn)(void*), const char*, uint32_t,
                                          void* arg, uint32_t, TaskHandle_t* out, int) {
  g_pending_task_fn = fn;
  g_pending_task_arg = arg;
  static int placeholder;
  if (out) *out = &placeholder;
  return 1;
}
extern "C" void __pico_notify_give(void);
static inline void xTaskNotifyGive(TaskHandle_t) { __pico_notify_give(); }
/* ulTaskNotifyTake is provided in pico_freertos_stubs.c (C linkage) so
 * plain-C emulation.c can link against the same implementation. */
extern "C" uint32_t ulTaskNotifyTake(int, TickType_t);
#ifndef pdTRUE
#define pdTRUE 1
#define pdFALSE 0
#endif
static inline int xTaskNotifyWait(uint32_t, uint32_t, uint32_t*, TickType_t) { return pdTRUE; }

/* --- ESP.* class stub --- */
struct EspStub {
  uint32_t getFreeHeap()          { return 200000; }
  uint32_t getMinFreeHeap()       { return 200000; }
  uint32_t getFreeSketchSpace()   { return 1000000; }
  uint32_t getCpuFreqMHz()        { return 150; }
  void     restart()              { }
};
static EspStub ESP;

/* --- Core-id / clock — Pico core has clock_get_hz() but no per-core id we use --- */
static inline int xPortGetCoreID() { return 0; }
static inline bool setCpuFrequencyMhz(uint32_t) { return true; }

/* --- Input glue (defined in pico_input.cpp, active when PICO_CUSTOM_INPUT). --- */
extern "C" void pico_input_init(void);
extern "C" unsigned char buttons_get(void);

/* --- SD card (defined in pico_sd.cpp) — first step of runtime ROM loader. --- */
extern "C" bool pico_sd_init(void);

/* --- Runtime game loads (defined per-game in *_runtime.cpp files) --- */
extern "C" bool runtime_load_pacman(void);
extern "C" bool runtime_load_galaga(void);

/* --- Boot-time text menu (defined in pico_menu.cpp) --- */
extern "C" void pico_menu_pick_and_load(void);

#endif
