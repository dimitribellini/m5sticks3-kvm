#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Launch UDP Netconsole receiver on port 6666 pinned to Core 1
bool netconsole_init(uint16_t port);

// Stop the Netconsole listener
void netconsole_stop(void);

// Telemetry counters
uint32_t netconsole_get_packets_received(void);
uint32_t netconsole_get_bytes_received(void);

#ifdef __cplusplus
}
#endif
