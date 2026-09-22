#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the 10KB FreeRTOS StreamBuffer and rolling circular log
void stream_buffer_subsystem_init(void);

// Send bytes into the FreeRTOS stream buffer (called from Core 1: TinyUSB / Netconsole)
size_t stream_buffer_write_bytes(const uint8_t *data, size_t length);

// Read bytes from the FreeRTOS stream buffer (called from Core 0: String Matcher / WebSocket)
size_t stream_buffer_read_bytes(uint8_t *data, size_t max_length, TickType_t wait_ticks);

// Append bytes to the 10KB circular rolling historical log
void rolling_log_append(const uint8_t *data, size_t length);

// Snapshot the current 10KB rolling log into a destination buffer (null-terminated)
size_t rolling_log_get_snapshot(char *dest, size_t max_dest_len);

// Clear the rolling log buffer
void rolling_log_clear(void);

// Global stream buffer handle
extern StreamBufferHandle_t g_stream_buffer;

#ifdef __cplusplus
}
#endif
