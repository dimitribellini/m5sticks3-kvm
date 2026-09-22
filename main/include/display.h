#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the 1.14" ST7789P3 display task and button handler pinned to Core 1
bool display_subsystem_init(void);

// Force switch to a specific display page (0: Dashboard, 1: Setup, 2: Diagnostics)
void display_set_page(uint8_t page);

// Cycle to next page
void display_next_page(void);

// Wake display from energy-saving sleep mode
void display_wake(void);

#ifdef __cplusplus
}
#endif
