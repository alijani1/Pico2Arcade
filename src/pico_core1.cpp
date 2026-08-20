/* pico_core1.cpp — earlephilhower framework's core-1 entry.
 *
 * Upstream Galagino calls xTaskCreatePinnedToCore(emulation_task, ...) in
 * setup(). Our compat stub captures the function pointer + arg; here we
 * pick them up and run the task on core 1. The "task" is an infinite loop
 * (prepare_emulation() then while(1) emulate_frame()), so we just call it
 * from setup1() and never return.
 */

#include <Arduino.h>

/* Storage owned by us; declared extern from pico_compat.h */
void (* volatile g_pending_task_fn)(void*) = nullptr;
void *          g_pending_task_arg = nullptr;
/* Second gate — set to 1 by pico_menu_pick_and_load once the user has
 * picked a game and the runtime pointers are populated. Prevents core 1
 * from starting the emulator against MCH_MENU while we're still in the
 * middle of showing our menu. */
volatile int g_menu_done = 0;

void setup1(void) {
  /* No Serial here — USB CDC may not be enumerated yet. */
  while (g_pending_task_fn == nullptr || g_menu_done == 0) {
    delay(5);
  }
  g_pending_task_fn(g_pending_task_arg);
  while (1) delay(1000);
}

void loop1(void) {
  /* Unused — setup1() never returns. */
}
