#include "app_stream_buffer.h"
#include "app_config.h"
#include "web_server.h"
#include <string.h>
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "stream_buf";

StreamBufferHandle_t g_stream_buffer = NULL;

// Circular rolling log buffer for 10KB crash snapshot
static uint8_t s_rolling_log[ROLLING_LOG_SIZE_BYTES];
static size_t s_log_head = 0;
static size_t s_log_count = 0;
static SemaphoreHandle_t s_log_mutex = NULL;

const char* alert_state_to_str(alert_state_t state) {
    switch (state) {
        case ALERT_STATE_OK: return "OK";
        case ALERT_STATE_VBUS_DROP: return "VBUS_DROP";
        case ALERT_STATE_SERIAL_LOST: return "SERIAL_LOST";
        case ALERT_STATE_OOM_DETECTED: return "OOM_DETECTED";
        case ALERT_STATE_KERNEL_PANIC: return "KERNEL_PANIC";
        default: return "UNKNOWN";
    }
}

void stream_buffer_subsystem_init(void) {
    if (g_stream_buffer == NULL) {
        g_stream_buffer = xStreamBufferCreate(STREAM_BUFFER_SIZE_BYTES, 1);
        if (g_stream_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate FreeRTOS StreamBuffer!");
        } else {
            ESP_LOGI(TAG, "Initialized 10KB FreeRTOS StreamBuffer");
        }
    }

    if (s_log_mutex == NULL) {
        s_log_mutex = xSemaphoreCreateMutex();
        memset(s_rolling_log, 0, sizeof(s_rolling_log));
        s_log_head = 0;
        s_log_count = 0;
    }
}

size_t stream_buffer_write_bytes(const uint8_t *data, size_t length) {
    if (g_stream_buffer == NULL || data == NULL || length == 0) {
        return 0;
    }

    // Append to rolling snapshot log
    rolling_log_append(data, length);

    // Broadcast directly to active WebSocket console clients
    web_server_broadcast_ws(data, length);

    // Send into FreeRTOS StreamBuffer for string matcher
    return xStreamBufferSend(g_stream_buffer, data, length, 0);
}

size_t stream_buffer_read_bytes(uint8_t *data, size_t max_length, TickType_t wait_ticks) {
    if (g_stream_buffer == NULL || data == NULL || max_length == 0) {
        return 0;
    }
    return xStreamBufferReceive(g_stream_buffer, data, max_length, wait_ticks);
}

void rolling_log_append(const uint8_t *data, size_t length) {
    if (s_log_mutex == NULL || data == NULL || length == 0) {
        return;
    }

    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (size_t i = 0; i < length; i++) {
            s_rolling_log[s_log_head] = data[i];
            s_log_head = (s_log_head + 1) % ROLLING_LOG_SIZE_BYTES;
            if (s_log_count < ROLLING_LOG_SIZE_BYTES) {
                s_log_count++;
            }
        }
        xSemaphoreGive(s_log_mutex);
    }
}

size_t rolling_log_get_snapshot(char *dest, size_t max_dest_len) {
    if (dest == NULL || max_dest_len == 0 || s_log_mutex == NULL) {
        return 0;
    }

    size_t bytes_copied = 0;
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t available = (s_log_count < (max_dest_len - 1)) ? s_log_count : (max_dest_len - 1);
        size_t start_idx = 0;

        if (s_log_count >= ROLLING_LOG_SIZE_BYTES) {
            // Buffer has wrapped around
            start_idx = (s_log_head + (ROLLING_LOG_SIZE_BYTES - available)) % ROLLING_LOG_SIZE_BYTES;
        } else {
            start_idx = (s_log_count >= available) ? (s_log_count - available) : 0;
        }

        for (size_t i = 0; i < available; i++) {
            dest[i] = (char)s_rolling_log[(start_idx + i) % ROLLING_LOG_SIZE_BYTES];
        }
        dest[available] = '\0';
        bytes_copied = available;

        xSemaphoreGive(s_log_mutex);
    }

    return bytes_copied;
}

void rolling_log_clear(void) {
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memset(s_rolling_log, 0, sizeof(s_rolling_log));
        s_log_head = 0;
        s_log_count = 0;
        xSemaphoreGive(s_log_mutex);
    }
}
