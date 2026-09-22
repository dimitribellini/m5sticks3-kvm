#include "config_manager.h"
#include "app_config.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "cfg_mgr";

static app_config_data_t s_active_config;
static bool s_initialized = false;

static void set_default_values(app_config_data_t *config) {
    memset(config, 0, sizeof(app_config_data_t));
    snprintf(config->wifi_ssid, sizeof(config->wifi_ssid), "%s", DEFAULT_WIFI_SSID);
    snprintf(config->wifi_pass, sizeof(config->wifi_pass), "%s", DEFAULT_WIFI_PASS);
    snprintf(config->ap_ssid, sizeof(config->ap_ssid), "%s", DEFAULT_AP_SSID);
    snprintf(config->ap_pass, sizeof(config->ap_pass), "%s", DEFAULT_AP_PASS);
    snprintf(config->wol_mac, sizeof(config->wol_mac), "%s", "00:00:00:00:00:00");
    snprintf(config->zabbix_server, sizeof(config->zabbix_server), "%s", "192.168.1.100");
    config->zabbix_port = ZABBIX_DEFAULT_PORT;
    snprintf(config->zabbix_host, sizeof(config->zabbix_host), "%s", DEFAULT_ZABBIX_HOST);
    snprintf(config->zabbix_item_key, sizeof(config->zabbix_item_key), "%s", DEFAULT_ZABBIX_ITEM_KEY);
    snprintf(config->http_user, sizeof(config->http_user), "%s", DEFAULT_HTTP_USER);
    snprintf(config->http_pass, sizeof(config->http_pass), "%s", DEFAULT_HTTP_PASS);
    config->netconsole_port = NETCONSOLE_UDP_PORT;
    config->is_configured = false;
}

void config_manager_reset_defaults(app_config_data_t *config) {
    set_default_values(config);
}

bool config_manager_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated or updated, re-initializing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: 0x%x", err);
        return false;
    }

    set_default_values(&s_active_config);

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t len = sizeof(s_active_config.wifi_ssid);
        nvs_get_str(handle, "wifi_ssid", s_active_config.wifi_ssid, &len);

        len = sizeof(s_active_config.wifi_pass);
        nvs_get_str(handle, "wifi_pass", s_active_config.wifi_pass, &len);

        len = sizeof(s_active_config.ap_ssid);
        nvs_get_str(handle, "ap_ssid", s_active_config.ap_ssid, &len);

        len = sizeof(s_active_config.ap_pass);
        nvs_get_str(handle, "ap_pass", s_active_config.ap_pass, &len);

        len = sizeof(s_active_config.wol_mac);
        nvs_get_str(handle, "wol_mac", s_active_config.wol_mac, &len);

        len = sizeof(s_active_config.zabbix_server);
        nvs_get_str(handle, "zabbix_srv", s_active_config.zabbix_server, &len);

        uint16_t zport = 0;
        if (nvs_get_u16(handle, "zabbix_prt", &zport) == ESP_OK) {
            s_active_config.zabbix_port = zport;
        }

        len = sizeof(s_active_config.zabbix_host);
        nvs_get_str(handle, "zabbix_hst", s_active_config.zabbix_host, &len);

        len = sizeof(s_active_config.zabbix_item_key);
        nvs_get_str(handle, "zabbix_key", s_active_config.zabbix_item_key, &len);

        len = sizeof(s_active_config.http_user);
        nvs_get_str(handle, "http_user", s_active_config.http_user, &len);

        len = sizeof(s_active_config.http_pass);
        nvs_get_str(handle, "http_pass", s_active_config.http_pass, &len);

        uint16_t nc_port = 0;
        if (nvs_get_u16(handle, "nc_port", &nc_port) == ESP_OK) {
            s_active_config.netconsole_port = nc_port;
        }

        uint8_t cfg_flag = 0;
        if (nvs_get_u8(handle, "is_cfg", &cfg_flag) == ESP_OK) {
            s_active_config.is_configured = (cfg_flag != 0);
        }

        nvs_close(handle);
        ESP_LOGI(TAG, "Config loaded from NVS ('%s'). Configured: %d", NVS_NAMESPACE, s_active_config.is_configured);
    } else {
        ESP_LOGI(TAG, "No existing config in NVS '%s'. Using defaults.", NVS_NAMESPACE);
    }

    s_initialized = true;
    return true;
}

const app_config_data_t* config_manager_get(void) {
    return &s_active_config;
}

bool config_manager_save(const app_config_data_t *config) {
    if (config == NULL) return false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: 0x%x", err);
        return false;
    }

    nvs_set_str(handle, "wifi_ssid", config->wifi_ssid);
    nvs_set_str(handle, "wifi_pass", config->wifi_pass);
    nvs_set_str(handle, "ap_ssid", config->ap_ssid);
    nvs_set_str(handle, "ap_pass", config->ap_pass);
    nvs_set_str(handle, "wol_mac", config->wol_mac);
    nvs_set_str(handle, "zabbix_srv", config->zabbix_server);
    nvs_set_u16(handle, "zabbix_prt", config->zabbix_port);
    nvs_set_str(handle, "zabbix_hst", config->zabbix_host);
    nvs_set_str(handle, "zabbix_key", config->zabbix_item_key);
    nvs_set_str(handle, "http_user", config->http_user);
    nvs_set_str(handle, "http_pass", config->http_pass);
    nvs_set_u16(handle, "nc_port", config->netconsole_port);
    nvs_set_u8(handle, "is_cfg", config->is_configured ? 1 : 0);

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        s_active_config = *config;
        ESP_LOGI(TAG, "Successfully committed configuration to NVS namespace '%s'", NVS_NAMESPACE);
        return true;
    }
    ESP_LOGE(TAG, "Failed to commit NVS transaction: 0x%x", err);
    return false;
}

bool config_manager_save_panic_dump(const char *dump_data, size_t length) {
    if (dump_data == NULL || length == 0) return false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for emergency dump: 0x%x", err);
        return false;
    }

    err = nvs_set_blob(handle, "panic_dump", dump_data, length);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Persisted %u bytes emergency panic dump to NVS key 'panic_dump'", (unsigned int)length);
        return true;
    }
    ESP_LOGE(TAG, "Failed to persist emergency dump to NVS: 0x%x", err);
    return false;
}

bool config_manager_read_panic_dump(char *dest, size_t max_len, size_t *out_len) {
    if (dest == NULL || max_len == 0) return false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;

    size_t required_size = 0;
    err = nvs_get_blob(handle, "panic_dump", NULL, &required_size);
    if (err != ESP_OK || required_size == 0) {
        nvs_close(handle);
        return false;
    }

    if (required_size >= max_len) {
        required_size = max_len - 1;
    }

    err = nvs_get_blob(handle, "panic_dump", dest, &required_size);
    nvs_close(handle);

    if (err == ESP_OK) {
        dest[required_size] = '\0';
        if (out_len) *out_len = required_size;
        return true;
    }
    return false;
}

bool config_manager_clear_panic_dump(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;

    nvs_erase_key(handle, "panic_dump");
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Cleared emergency panic dump key from NVS");
    return true;
}
