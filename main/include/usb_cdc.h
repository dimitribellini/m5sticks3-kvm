#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the TinyUSB CDC-ACM stack and launch Core 1 polling task
bool usb_cdc_init(void);

// Queue bytes to transmit to the host server console (from WebSocket client)
size_t usb_cdc_send_to_host(const uint8_t *data, size_t length);

// Check if host has connected and opened the CDC terminal
bool usb_cdc_is_connected(void);

// Total RX/TX byte counters for telemetry
uint32_t usb_cdc_get_rx_bytes(void);
uint32_t usb_cdc_get_tx_bytes(void);

#ifdef __cplusplus
}
#endif
