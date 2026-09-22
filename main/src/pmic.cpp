#include "pmic.h"
#include "app_config.h"
#include "zabbix.h"
#include <M5Unified.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "pmic";

static float s_vbus_voltage = 5.0f;
static int32_t s_battery_level = 100;
static bool s_is_charging = false;
static bool s_vbus_present = true;
static alert_state_t s_current_alert = ALERT_STATE_OK;

static void pmic_task(void *pvParameters) {
    ESP_LOGI(TAG, "PMIC supervisor task running on Core %d", xPortGetCoreID());

    bool previous_vbus_ok = true;

    while (1) {
        // Query PMIC through M5Unified (getVBUSVoltage returns millivolts)
        s_vbus_voltage = (float)M5.Power.getVBUSVoltage() / 1000.0f;
        s_battery_level = M5.Power.getBatteryLevel();
        s_is_charging = M5.Power.isCharging();
        s_vbus_present = (s_vbus_voltage >= VBUS_DROP_THRESHOLD_VOLTS);

        if (previous_vbus_ok && !s_vbus_present) {
            ESP_LOGW(TAG, "ALERT: VBUS voltage dropped to %.2fV (< %.1fV)! Initiating emergency alert sequence...",
                     s_vbus_voltage, VBUS_DROP_THRESHOLD_VOLTS);
            s_current_alert = ALERT_STATE_VBUS_DROP;
            previous_vbus_ok = false;

            // Dispatch emergency alert to Zabbix / NVS queue
            zabbix_trigger_alert(ALERT_STATE_VBUS_DROP, "Host VBUS dropped below 4.0V: Server power loss detected");
        } else if (!previous_vbus_ok && s_vbus_present) {
            ESP_LOGI(TAG, "VBUS power restored: %.2fV. Resetting alert state to OK.", s_vbus_voltage);
            if (s_current_alert == ALERT_STATE_VBUS_DROP) {
                s_current_alert = ALERT_STATE_OK;
            }
            previous_vbus_ok = true;
        }

        vTaskDelay(pdMS_TO_TICKS(PMIC_POLL_INTERVAL_MS));
    }
}

bool pmic_supervisor_init(void) {
    BaseType_t res = xTaskCreatePinnedToCore(
        pmic_task,
        "task_pmic",
        3072,
        NULL,
        PRIORITY_TASK_PMIC,
        NULL,
        CORE_HARDWARE_INGESTION
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to launch task_pmic on Core %d", CORE_HARDWARE_INGESTION);
        return false;
    }

    ESP_LOGI(TAG, "PMIC supervisor initialized");
    return true;
}

float pmic_get_vbus_voltage(void) {
    return s_vbus_voltage;
}

int32_t pmic_get_battery_level(void) {
    return s_battery_level;
}

bool pmic_is_charging(void) {
    return s_is_charging;
}

bool pmic_is_vbus_connected(void) {
    return s_vbus_present;
}

alert_state_t pmic_get_alert_state(void) {
    return s_current_alert;
}

void pmic_set_alert_state(alert_state_t state) {
    s_current_alert = state;
}

void pmic_power_off(void) {
    ESP_LOGW(TAG, "Executing device power off via M5.Power.powerOff()");
    M5.Power.powerOff();
}
