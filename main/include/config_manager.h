#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char ap_ssid[64];
    char ap_pass[64];
    char wol_mac[18];            // Format: "XX:XX:XX:XX:XX:XX"
    char zabbix_server[64];
    uint16_t zabbix_port;
    char zabbix_host[64];
    char zabbix_item_key[64];
    char http_user[32];
    char http_pass[32];
    uint16_t netconsole_port;
    bool is_configured;
} app_config_data_t;

// Initialize NVS subsystem and load configuration from "blackbox_oob" namespace
bool config_manager_init(void);

// Get current active configuration pointer
const app_config_data_t* config_manager_get(void);

// Update and persist configuration to NVS
bool config_manager_save(const app_config_data_t *config);

// Reset configuration back to compile-time defaults
void config_manager_reset_defaults(app_config_data_t *config);

// Save emergency panic / blackout dump to NVS namespace "blackbox_oob"
bool config_manager_save_panic_dump(const char *dump_data, size_t length);

// Read pending panic dump from NVS if one exists from a previous crash/blackout
bool config_manager_read_panic_dump(char *dest, size_t max_len, size_t *out_len);

// Clear the pending panic dump from NVS after successful transmission
bool config_manager_clear_panic_dump(void);

#ifdef __cplusplus
}
#endif
