#include "string_matcher.h"
#include "app_stream_buffer.h"
#include "app_config.h"
#include "pmic.h"
#include "zabbix.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "str_match";

static TaskHandle_t s_task_handle = NULL;
static bool s_running = false;

static void evaluate_line(const char *line) {
    if (line == NULL || line[0] == '\0') return;

    if (strstr(line, "Kernel panic") != NULL || strstr(line, "BUG: unable to handle kernel") != NULL) {
        ESP_LOGE(TAG, "ALERT: Kernel Panic pattern matched: %s", line);
        pmic_set_alert_state(ALERT_STATE_KERNEL_PANIC);
        zabbix_trigger_alert(ALERT_STATE_KERNEL_PANIC, line);
    } else if (strstr(line, "Out of memory: Kill process") != NULL || strstr(line, "oom-killer") != NULL) {
        ESP_LOGE(TAG, "ALERT: OOM Killer pattern matched: %s", line);
        pmic_set_alert_state(ALERT_STATE_OOM_DETECTED);
        zabbix_trigger_alert(ALERT_STATE_OOM_DETECTED, line);
    } else if (strstr(line, "Call Trace:") != NULL) {
        ESP_LOGW(TAG, "Kernel Call Trace detected: %s", line);
        if (pmic_get_alert_state() == ALERT_STATE_OK) {
            pmic_set_alert_state(ALERT_STATE_KERNEL_PANIC);
            zabbix_trigger_alert(ALERT_STATE_KERNEL_PANIC, line);
        }
    }
}

static void string_matcher_task(void *pvParameters) {
    ESP_LOGI(TAG, "String Matcher task started on Core %d", xPortGetCoreID());

    char line_buf[512];
    size_t line_len = 0;
    uint8_t rx_byte = 0;

    while (s_running) {
        // Read byte by byte with a timeout
        size_t n = stream_buffer_read_bytes(&rx_byte, 1, pdMS_TO_TICKS(100));
        if (n > 0) {
            if (rx_byte == '\n' || rx_byte == '\r') {
                if (line_len > 0) {
                    line_buf[line_len] = '\0';
                    evaluate_line(line_buf);
                    line_len = 0;
                }
            } else {
                if (line_len < sizeof(line_buf) - 1) {
                    line_buf[line_len++] = (char)rx_byte;
                } else {
                    // Line buffer overflow, evaluate chunk
                    line_buf[line_len] = '\0';
                    evaluate_line(line_buf);
                    line_len = 0;
                }
            }
        }
    }

    s_task_handle = NULL;
    vTaskDelete(NULL);
}

bool string_matcher_init(void) {
    s_running = true;
    BaseType_t res = xTaskCreatePinnedToCore(
        string_matcher_task,
        "task_str_match",
        4096,
        NULL,
        PRIORITY_TASK_STRING_MATCH,
        &s_task_handle,
        CORE_NETWORK_LOGIC
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to launch task_str_match on Core %d", CORE_NETWORK_LOGIC);
        s_running = false;
        return false;
    }

    return true;
}

void string_matcher_stop(void) {
    s_running = false;
}
