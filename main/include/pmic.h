#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize PMIC polling task on Core 1
bool pmic_supervisor_init(void);

// Get current telemetry values
float pmic_get_vbus_voltage(void);
int32_t pmic_get_battery_level(void);
bool pmic_is_charging(void);
bool pmic_is_vbus_connected(void);

// Current system alert state
alert_state_t pmic_get_alert_state(void);
void pmic_set_alert_state(alert_state_t state);

// Trigger device power down
void pmic_power_off(void);

#ifdef __cplusplus
}
#endif
