#include "wifi_manager.h"
#include "config_manager.h"
#include "app_config.h"
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

static bool s_is_connected = false;
static bool s_is_ap_mode = false;
static bool s_wifi_started = false;
static char s_current_ip[32] = {0};
static int8_t s_rssi = 0;
static int s_retry_count = 0;
static const int MAX_STA_RETRIES = 10;

static void fallback_to_softap_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "Spawning SoftAP fallback from async worker...");
    wifi_manager_start_softap();
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi Station started, attempting to connect...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        s_current_ip[0] = '\0';
        if (s_retry_count < MAX_STA_RETRIES) {
            s_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi disconnected. Retry %d/%d...", s_retry_count, MAX_STA_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "Wi-Fi connection failed after %d retries. Spawning SoftAP fallback...", MAX_STA_RETRIES);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            xTaskCreate(fallback_to_softap_task, "ap_fallback", 3072, NULL, 3, NULL);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_current_ip, sizeof(s_current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_is_connected = true;
        s_is_ap_mode = false;
        s_retry_count = 0;
        ESP_LOGI(TAG, "Got IP address: %s", s_current_ip);

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_rssi = ap_info.rssi;
        }

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        s_is_ap_mode = true;
        strncpy(s_current_ip, "192.168.4.1", sizeof(s_current_ip) - 1);
        ESP_LOGI(TAG, "SoftAP active at IP: %s", s_current_ip);
    }
}

bool wifi_manager_init(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    return true;
}

bool wifi_manager_connect_sta(void) {
    const app_config_data_t *cfg = config_manager_get();

    if (cfg->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "Cannot start Station mode: No SSID configured in NVS.");
        return false;
    }

    ESP_LOGI(TAG, "Connecting to Station SSID '%s'...", cfg->wifi_ssid);
    wifi_config_t wifi_sta_cfg;
    memset(&wifi_sta_cfg, 0, sizeof(wifi_sta_cfg));
    strncpy((char *)wifi_sta_cfg.sta.ssid, cfg->wifi_ssid, sizeof(wifi_sta_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_sta_cfg.sta.password, cfg->wifi_pass, sizeof(wifi_sta_cfg.sta.password) - 1);
    wifi_sta_cfg.sta.threshold.authmode = (strlen(cfg->wifi_pass) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_sta_cfg.sta.pmf_cfg.capable = true;
    wifi_sta_cfg.sta.pmf_cfg.required = false;

    s_retry_count = 0;
    s_is_ap_mode = false;
    s_is_connected = false;
    s_current_ip[0] = '\0';

    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifi_started = false;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;
    return true;
}

bool wifi_manager_start(void) {
    const app_config_data_t *cfg = config_manager_get();

    if (cfg->wifi_ssid[0] != '\0') {
        return wifi_manager_connect_sta();
    }

    // No credentials configured yet -> start SoftAP onboarding
    ESP_LOGI(TAG, "No Wi-Fi credentials in NVS. Starting SoftAP onboarding...");
    wifi_manager_start_softap();
    return true;
}

void wifi_manager_start_softap(void) {
    const app_config_data_t *cfg = config_manager_get();

    wifi_config_t wifi_ap_cfg;
    memset(&wifi_ap_cfg, 0, sizeof(wifi_ap_cfg));
    strncpy((char *)wifi_ap_cfg.ap.ssid, cfg->ap_ssid, sizeof(wifi_ap_cfg.ap.ssid) - 1);
    wifi_ap_cfg.ap.ssid_len = strlen(cfg->ap_ssid);
    strncpy((char *)wifi_ap_cfg.ap.password, cfg->ap_pass, sizeof(wifi_ap_cfg.ap.password) - 1);
    wifi_ap_cfg.ap.max_connection = 4;
    wifi_ap_cfg.ap.authmode = (strlen(cfg->ap_pass) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifi_started = false;
    }

    // Use APSTA mode so STA scanning for nearby networks remains functional
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;

    s_is_ap_mode = true;
    s_is_connected = false;
    strncpy(s_current_ip, "192.168.4.1", sizeof(s_current_ip) - 1);
    ESP_LOGI(TAG, "SoftAP started: SSID='%s' IP=192.168.4.1 (APSTA mode)", cfg->ap_ssid);
}

void wifi_manager_toggle_ap_mode(void) {
    if (s_is_ap_mode) {
        const app_config_data_t *cfg = config_manager_get();
        if (cfg->wifi_ssid[0] != '\0') {
            ESP_LOGI(TAG, "Toggling to Station mode (SSID='%s')...", cfg->wifi_ssid);
            wifi_manager_connect_sta();
        } else {
            ESP_LOGW(TAG, "Cannot switch to Station mode: No Wi-Fi SSID configured in NVS!");
        }
    } else {
        ESP_LOGI(TAG, "Toggling to SoftAP mode...");
        wifi_manager_start_softap();
    }
}

bool wifi_manager_get_ip_str(char *dest, size_t max_len) {
    if (dest == NULL || max_len == 0) return false;
    strncpy(dest, s_current_ip, max_len - 1);
    dest[max_len - 1] = '\0';
    return (s_current_ip[0] != '\0');
}

bool wifi_manager_is_connected(void) {
    return s_is_connected;
}

bool wifi_manager_is_ap_mode(void) {
    return s_is_ap_mode;
}

int8_t wifi_manager_get_rssi(void) {
    if (s_is_connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_rssi = ap_info.rssi;
        }
    }
    return s_rssi;
}

size_t wifi_manager_scan_json(char *dest, size_t max_len) {
    if (dest == NULL || max_len < 16) return 0;

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;

    esp_err_t scan_res = esp_wifi_scan_start(&scan_cfg, true); // Blocking scan
    if (scan_res != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: 0x%x", scan_res);
        snprintf(dest, max_len, "[]");
        return 2;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records) {
        snprintf(dest, max_len, "[]");
        return 2;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    size_t offset = 0;
    offset += snprintf(dest + offset, max_len - offset, "[");

    for (int i = 0; i < ap_count; i++) {
        if (offset + 128 >= max_len) break;
        offset += snprintf(dest + offset, max_len - offset,
                           "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                           (i > 0) ? "," : "",
                           (char *)ap_records[i].ssid,
                           ap_records[i].rssi,
                           ap_records[i].authmode);
    }

    offset += snprintf(dest + offset, max_len - offset, "]");
    free(ap_records);

    return offset;
}
