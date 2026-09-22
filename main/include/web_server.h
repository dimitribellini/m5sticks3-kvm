#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Launch embedded HTTP server and WebSocket console bridge on Core 0
bool web_server_init(void);

// Stop web server
void web_server_stop(void);

// Broadcast raw bytes to all connected WebSocket terminal clients
void web_server_broadcast_ws(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif
