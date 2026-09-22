#include "web_server.h"
#include "app_config.h"
#include "auth.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "wol.h"
#include "pmic.h"
#include "usb_cdc.h"
#include "netconsole.h"
#include "app_stream_buffer.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "web_server";

// Embedded binary assets symbols (from ESP-IDF EMBED_TXTFILES)
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t xterm_js_start[]   asm("_binary_xterm_js_start");
extern const uint8_t xterm_js_end[]     asm("_binary_xterm_js_end");

static httpd_handle_t s_server = NULL;
static int s_ws_clients[4] = {-1, -1, -1, -1};
static const int MAX_WS_CLIENTS = 4;
static SemaphoreHandle_t s_ws_mutex = NULL;

static void register_ws_client(int fd) {
    if (s_ws_mutex) xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_clients[i] == fd) {
            if (s_ws_mutex) xSemaphoreGive(s_ws_mutex);
            return;
        }
    }
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_clients[i] == -1) {
            s_ws_clients[i] = fd;
            ESP_LOGI(TAG, "Registered WS client fd: %d (slot %d)", fd, i);
            if (s_ws_mutex) xSemaphoreGive(s_ws_mutex);
            return;
        }
    }
    // All slots full, replace first
    s_ws_clients[0] = fd;
    if (s_ws_mutex) xSemaphoreGive(s_ws_mutex);
}

static void unregister_ws_client(int fd) {
    if (s_ws_mutex) xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_clients[i] == fd) {
            s_ws_clients[i] = -1;
            ESP_LOGI(TAG, "Unregistered WS client fd: %d", fd);
            break;
        }
    }
    if (s_ws_mutex) xSemaphoreGive(s_ws_mutex);
}

void web_server_broadcast_ws(const uint8_t *data, size_t length) {
    if (s_server == NULL || data == NULL || length == 0) return;

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.final = true;
    ws_pkt.fragmented = false;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t *)data;
    ws_pkt.len = length;

    if (s_ws_mutex) xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        int fd = s_ws_clients[i];
        if (fd != -1) {
            if (httpd_ws_get_fd_info(s_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
                esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &ws_pkt);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to send WS to fd %d: 0x%x", fd, err);
                    if (err == ESP_FAIL || err == ESP_ERR_INVALID_ARG) {
                        s_ws_clients[i] = -1;
                    }
                }
            } else {
                s_ws_clients[i] = -1;
            }
        }
    }
    if (s_ws_mutex) xSemaphoreGive(s_ws_mutex);
}

// Handler: GET / (Embedded index.html)
static esp_err_t root_get_handler(httpd_req_t *req) {
    if (!auth_check_http_basic(req)) return ESP_OK;

    size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

// Handler: GET /xterm.js
static esp_err_t xterm_js_get_handler(httpd_req_t *req) {
    size_t len = xterm_js_end - xterm_js_start;
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, (const char *)xterm_js_start, len);
}

// Handler: WebSocket /ws/terminal
static esp_err_t ws_terminal_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // Handshake
        int fd = httpd_req_to_sockfd(req);
        register_ws_client(fd);
        ESP_LOGI(TAG, "WebSocket handshake completed for fd: %d", fd);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t buf[256];
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = buf;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf) - 1);
    if (ret != ESP_OK) {
        unregister_ws_client(httpd_req_to_sockfd(req));
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        unregister_ws_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    // Keep active client registered
    int fd = httpd_req_to_sockfd(req);
    register_ws_client(fd);

    if (ws_pkt.len > 0) {
        buf[ws_pkt.len] = '\0';
        // Route keystrokes received from browser client directly to host console via TinyUSB
        usb_cdc_send_to_host(buf, ws_pkt.len);
    }
    return ESP_OK;
}

// Helper: Extract string from JSON without external parser
static bool extract_json_str(const char *json, const char *key, char *dest, size_t max_len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = end - p;
    if (len >= max_len) len = max_len - 1;
    memcpy(dest, p, len);
    dest[len] = '\0';
    return true;
}

// Helper: Extract integer from JSON without external parser
static bool extract_json_int(const char *json, const char *key, int *dest) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    *dest = atoi(p);
    return true;
}

// Handler: GET /api/config
static esp_err_t api_config_get_handler(httpd_req_t *req) {
    if (!auth_check_http_basic(req)) return ESP_OK;

    const app_config_data_t *cfg = config_manager_get();
    char cur_ip[32] = {0};
    wifi_manager_get_ip_str(cur_ip, sizeof(cur_ip));

    char json_str[768];
    snprintf(json_str, sizeof(json_str),
             "{\"wifi_ssid\":\"%s\",\"has_wifi_pass\":%s,\"wol_mac\":\"%s\",\"zabbix_server\":\"%s\",\"zabbix_port\":%u,\"zabbix_host\":\"%s\",\"zabbix_item_key\":\"%s\",\"http_user\":\"%s\",\"netconsole_port\":%u,\"is_configured\":%s,\"is_ap\":%s,\"is_connected\":%s,\"ip\":\"%s\"}",
             cfg->wifi_ssid,
             (cfg->wifi_pass[0] != '\0') ? "true" : "false",
             cfg->wol_mac, cfg->zabbix_server, (unsigned int)cfg->zabbix_port,
             cfg->zabbix_host, cfg->zabbix_item_key, cfg->http_user, (unsigned int)cfg->netconsole_port,
             cfg->is_configured ? "true" : "false",
             wifi_manager_is_ap_mode() ? "true" : "false",
             wifi_manager_is_connected() ? "true" : "false",
             cur_ip);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    return ESP_OK;
}

// Handler: POST /api/config
static esp_err_t api_config_post_handler(httpd_req_t *req) {
    if (!auth_check_http_basic(req)) return ESP_OK;

    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    app_config_data_t new_cfg = *config_manager_get();

    extract_json_str(buf, "wifi_ssid", new_cfg.wifi_ssid, sizeof(new_cfg.wifi_ssid));

    char pass_tmp[65] = {0};
    if (extract_json_str(buf, "wifi_pass", pass_tmp, sizeof(pass_tmp)) && pass_tmp[0] != '\0') {
        strncpy(new_cfg.wifi_pass, pass_tmp, sizeof(new_cfg.wifi_pass) - 1);
        new_cfg.wifi_pass[sizeof(new_cfg.wifi_pass) - 1] = '\0';
    }

    extract_json_str(buf, "wol_mac", new_cfg.wol_mac, sizeof(new_cfg.wol_mac));
    extract_json_str(buf, "zabbix_server", new_cfg.zabbix_server, sizeof(new_cfg.zabbix_server));

    int val = 0;
    if (extract_json_int(buf, "zabbix_port", &val)) new_cfg.zabbix_port = (uint16_t)val;

    extract_json_str(buf, "zabbix_host", new_cfg.zabbix_host, sizeof(new_cfg.zabbix_host));
    extract_json_str(buf, "zabbix_item_key", new_cfg.zabbix_item_key, sizeof(new_cfg.zabbix_item_key));
    extract_json_str(buf, "http_user", new_cfg.http_user, sizeof(new_cfg.http_user));

    char http_pass_tmp[33] = {0};
    if (extract_json_str(buf, "http_pass", http_pass_tmp, sizeof(http_pass_tmp)) && http_pass_tmp[0] != '\0') {
        strncpy(new_cfg.http_pass, http_pass_tmp, sizeof(new_cfg.http_pass) - 1);
        new_cfg.http_pass[sizeof(new_cfg.http_pass) - 1] = '\0';
    }

    new_cfg.is_configured = true;

    bool ok = config_manager_save(&new_cfg);
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"status\":\"error\",\"error\":\"NVS save failed\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

// Handler: GET /api/scan_wifi
static esp_err_t api_scan_wifi_handler(httpd_req_t *req) {
    if (!auth_check_http_basic(req)) return ESP_OK;

    char *json_buf = (char *)malloc(2048);
    if (!json_buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t len = wifi_manager_scan_json(json_buf, 2048);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_buf, len);

    free(json_buf);
    return ESP_OK;
}

// Handler: POST /api/wol
static esp_err_t api_wol_handler(httpd_req_t *req) {
    if (!auth_check_http_basic(req)) return ESP_OK;

    const app_config_data_t *cfg = config_manager_get();
    bool sent = wol_send_magic_packet(cfg->wol_mac);

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"%s\",\"mac\":\"%s\"}", sent ? "ok" : "error", cfg->wol_mac);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// Handler: GET /api/logs
static esp_err_t api_logs_handler(httpd_req_t *req) {
    if (!auth_check_http_basic(req)) return ESP_OK;

    char *log_buf = (char *)malloc(ROLLING_LOG_SIZE_BYTES + 1);
    if (!log_buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t len = rolling_log_get_snapshot(log_buf, ROLLING_LOG_SIZE_BYTES);

    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (strstr(query, "download=1")) {
            httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"m5sticks3_kvm_logs.txt\"");
        }
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, log_buf, len);

    free(log_buf);
    return ESP_OK;
}

// Handler: GET /api/diagnostics
static esp_err_t api_diagnostics_handler(httpd_req_t *req) {
    if (!auth_check_http_basic(req)) return ESP_OK;

    char json_str[768];
    snprintf(json_str, sizeof(json_str),
             "{\"vbus_voltage\":%.2f,\"battery_level\":%ld,\"is_charging\":%s,\"alert_state\":\"%s\",\"usb_connected\":%s,\"netconsole_packets\":%u,\"usb_rx_bytes\":%u,\"usb_tx_bytes\":%u,\"free_ram\":%u,\"free_psram\":%u,\"uptime_sec\":%lld,\"app_version\":\"%s\",\"build_date\":\"%s\"}",
             pmic_get_vbus_voltage(),
             (long)pmic_get_battery_level(),
             pmic_is_charging() ? "true" : "false",
             alert_state_to_str(pmic_get_alert_state()),
             usb_cdc_is_connected() ? "true" : "false",
             (unsigned int)netconsole_get_packets_received(),
             (unsigned int)usb_cdc_get_rx_bytes(),
             (unsigned int)usb_cdc_get_tx_bytes(),
             (unsigned int)esp_get_free_heap_size(),
             (unsigned int)esp_get_free_internal_heap_size(),
             (long long)(esp_timer_get_time() / 1000000),
             APP_VERSION,
             APP_BUILD_TIMESTAMP);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    return ESP_OK;
}

// Handler: POST /api/reboot
static esp_err_t api_reboot_handler(httpd_req_t *req) {
    if (!auth_check_http_basic(req)) return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"rebooting\"}", HTTPD_RESP_USE_STRLEN);

    ESP_LOGW(TAG, "Reboot triggered via Web API");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// Handler: POST /api/toggle_ap
static esp_err_t api_toggle_ap_handler(httpd_req_t *req) {
    if (!auth_check_http_basic(req)) return ESP_OK;

    wifi_manager_toggle_ap_mode();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

// Handler: POST /api/connect_wifi
static esp_err_t api_connect_wifi_handler(httpd_req_t *req) {
    if (!auth_check_http_basic(req)) return ESP_OK;

    bool ok = wifi_manager_connect_sta();
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"%s\"}", ok ? "ok" : "error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

bool web_server_init(void) {
    if (s_ws_mutex == NULL) {
        s_ws_mutex = xSemaphoreCreateMutex();
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_SERVER_PORT;
    config.ctrl_port = 32768;
    config.max_open_sockets = 7;
    config.max_uri_handlers = 16;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP/WebSocket server on port %d on Core %d", config.server_port, CORE_NETWORK_LOGIC);

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
        return false;
    }

    // Helper to register endpoints with zero-initialized httpd_uri_t
    auto register_uri = [](httpd_handle_t server, const char *uri, httpd_method_t method,
                           esp_err_t (*handler)(httpd_req_t *), bool is_ws = false) {
        httpd_uri_t u = {};
        u.uri = uri;
        u.method = method;
        u.handler = handler;
        u.user_ctx = NULL;
        u.is_websocket = is_ws;
        esp_err_t err = httpd_register_uri_handler(server, &u);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI %s (method %d): 0x%x", uri, method, err);
        }
    };

    register_uri(s_server, "/", HTTP_GET, root_get_handler);
    register_uri(s_server, "/xterm.js", HTTP_GET, xterm_js_get_handler);
    register_uri(s_server, WS_TERMINAL_PATH, HTTP_GET, ws_terminal_handler, true);
    register_uri(s_server, "/api/config", HTTP_GET, api_config_get_handler);
    register_uri(s_server, "/api/config", HTTP_POST, api_config_post_handler);
    register_uri(s_server, "/api/scan_wifi", HTTP_GET, api_scan_wifi_handler);
    register_uri(s_server, "/api/wol", HTTP_POST, api_wol_handler);
    register_uri(s_server, "/api/logs", HTTP_GET, api_logs_handler);
    register_uri(s_server, "/api/diagnostics", HTTP_GET, api_diagnostics_handler);
    register_uri(s_server, "/api/reboot", HTTP_POST, api_reboot_handler);
    register_uri(s_server, "/api/toggle_ap", HTTP_POST, api_toggle_ap_handler);
    register_uri(s_server, "/api/connect_wifi", HTTP_POST, api_connect_wifi_handler);

    ESP_LOGI(TAG, "Registered HTTP and WebSocket endpoints successfully");
    return true;
}

void web_server_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
