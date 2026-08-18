/*
 * STANDARD main.c for the "empty" (bare-metal) project - Simplicity SDK / xG26.
 * PASTE this entire content into the project's main.c.
 * Key point: the while(1) loop must call app_process_action().
 */

#include "sl_component_catalog.h"
#include "sl_main_init.h"            // SDK 2025.12: replaces sl_system_init.h (old name)
#include "app.h"
#if defined(SL_CATALOG_KERNEL_PRESENT)
#include "sl_main_kernel.h"
#else
#include "sl_main_process_action.h"  // SDK 2025.12: replaces sl_system_process_action.h
#endif

int main(void)
{
  // Initialize the device, system, services, protocol stacks (new name: sl_main_init)
  sl_main_init();

  // Initialize the application (runs once) -> the 2 startup printf lines happen here
  app_init();

#if defined(SL_CATALOG_KERNEL_PRESENT)
  // If the project has an RTOS (kernel): tasks created in app_init() will run.
  sl_main_kernel_start();
#else
  while (1) {
    // MUST keep this: lets Silicon Labs' components run (new name)
    sl_main_process_action();

    // >>> APPLICATION LOOP: calls the repeating handler (like Arduino's loop()) <<<
    app_process_action();
  }
#endif
}
