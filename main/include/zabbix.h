#pragma once

#include <stdbool.h>
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize Zabbix task and alert queue on Core 0
bool zabbix_subsystem_init(void);

// Trigger an alert to Zabbix (with 10KB rolling log snapshot attached)
bool zabbix_trigger_alert(alert_state_t alert_type, const char *message);

// Check if any pending panic dump exists in NVS from a previous blackout/crash and send it
void zabbix_check_and_drain_nvs_dump(void);

// Total alert telemetry counter
uint32_t zabbix_get_alerts_sent(void);
uint32_t zabbix_get_alerts_failed(void);

#ifdef __cplusplus
}
#endif
