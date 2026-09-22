#include "auth.h"
#include "config_manager.h"
#include <string.h>
#include <stdio.h>
#include "mbedtls/base64.h"
#include "esp_log.h"

static const char *TAG = "auth";

bool auth_check_http_basic(httpd_req_t *req) {
    if (req == NULL) return false;

    const app_config_data_t *cfg = config_manager_get();

    // If username and password are not set, allow access
    if (cfg->http_user[0] == '\0' && cfg->http_pass[0] == '\0') {
        return true;
    }

    char auth_hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) == ESP_OK) {
        if (strncmp(auth_hdr, "Basic ", 6) == 0) {
            const char *b64_str = auth_hdr + 6;
            size_t b64_len = strlen(b64_str);

            unsigned char decoded[128] = {0};
            size_t out_len = 0;
            int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &out_len,
                                            (const unsigned char *)b64_str, b64_len);

            if (ret == 0 && out_len > 0) {
                decoded[out_len] = '\0';
                char expected[96];
                snprintf(expected, sizeof(expected), "%s:%s", cfg->http_user, cfg->http_pass);

                if (strcmp((char *)decoded, expected) == 0) {
                    return true;
                }
            }
        }
    }

    // Authentication failed: send 401
    ESP_LOGW(TAG, "Unauthorized request for URI: %s", req->uri);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"M5StickS3 KVM\"");
    httpd_resp_send(req, "401 Unauthorized: Valid credentials required\n", HTTPD_RESP_USE_STRLEN);

    return false;
}
