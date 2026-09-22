#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize Wi-Fi subsystem (handles Station and SoftAP fallback)
bool wifi_manager_init(void);

// Connect using stored credentials or start SoftAP if unconfigured
bool wifi_manager_start(void);

// Force switch to SoftAP mode
void wifi_manager_start_softap(void);

// Connect directly to Station mode using configured credentials
bool wifi_manager_connect_sta(void);

// Toggle between Station and SoftAP mode
void wifi_manager_toggle_ap_mode(void);

// Get current IPv4 address string (Station IP or SoftAP 192.168.4.1)
bool wifi_manager_get_ip_str(char *dest, size_t max_len);

// Connection status
bool wifi_manager_is_connected(void);
bool wifi_manager_is_ap_mode(void);
int8_t wifi_manager_get_rssi(void);

// Scan nearby access points and format into JSON string for Web UI
size_t wifi_manager_scan_json(char *dest, size_t max_len);

#ifdef __cplusplus
}
#endif
