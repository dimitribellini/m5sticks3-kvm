#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Check request Authorization header against stored credentials.
// Sends HTTP 401 Unauthorized if invalid or missing.
bool auth_check_http_basic(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
