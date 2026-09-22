#include "usb_cdc.h"
#include "app_stream_buffer.h"
#include "app_config.h"
#include "pmic.h"
#include "zabbix.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"

static const char *TAG = "usb_cdc";

static RingbufHandle_t s_tx_ringbuf = NULL;
static bool s_host_connected = false;
static uint32_t s_rx_byte_count = 0;
static uint32_t s_tx_byte_count = 0;

static void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event) {
    uint8_t buf[256];
    size_t rx_size = 0;
    while (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buf, sizeof(buf), &rx_size) == ESP_OK && rx_size > 0) {
        s_rx_byte_count += rx_size;
        stream_buffer_write_bytes(buf, rx_size);
    }
}

static void tinyusb_cdc_line_state_callback(int itf, cdcacm_event_t *event) {
    int dtr = event->line_state_changed_data.dtr;
    int rts = event->line_state_changed_data.rts;
    bool now_conn = (dtr || rts);
    ESP_LOGI(TAG, "Line state changed: DTR=%d, RTS=%d (Now: %d, Prev: %d)", dtr, rts, now_conn, s_host_connected);

    if (s_host_connected && !now_conn) {
        ESP_LOGW(TAG, "Host closed serial port /dev/ttyACM0!");
        s_host_connected = false;
        if (pmic_get_alert_state() == ALERT_STATE_OK) {
            pmic_set_alert_state(ALERT_STATE_SERIAL_LOST);
        }
        zabbix_trigger_alert(ALERT_STATE_SERIAL_LOST, "Host closed /dev/ttyACM0 (serial-getty stopped or host powered down)");
    } else if (!s_host_connected && now_conn) {
        ESP_LOGI(TAG, "Host opened serial port /dev/ttyACM0!");
        s_host_connected = true;
        if (pmic_get_alert_state() == ALERT_STATE_SERIAL_LOST) {
            pmic_set_alert_state(ALERT_STATE_OK);
        }
    }
}

static void tinyusb_task(void *pvParameters) {
    ESP_LOGI(TAG, "TinyUSB task running on Core %d", xPortGetCoreID());

    uint8_t tx_temp[128];
    bool last_tud_conn = false;

    while (1) {
        bool tud_conn = tud_cdc_n_connected(0);
        if (last_tud_conn != tud_conn) {
            last_tud_conn = tud_conn;
            if (!tud_conn && s_host_connected) {
                s_host_connected = false;
                ESP_LOGW(TAG, "Host USB connection suspended or unmounted!");
                if (pmic_get_alert_state() == ALERT_STATE_OK) {
                    pmic_set_alert_state(ALERT_STATE_SERIAL_LOST);
                }
                zabbix_trigger_alert(ALERT_STATE_SERIAL_LOST, "Host USB connection lost / suspended (server power off)");
            }
        }

        // Check if there are keystrokes from WebSockets to send to host serial
        if (s_tx_ringbuf) {
            size_t item_size = 0;
            uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(
                s_tx_ringbuf,
                &item_size,
                pdMS_TO_TICKS(10),
                sizeof(tx_temp)
            );

            if (item != NULL && item_size > 0) {
                tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, item, item_size);
                tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(10));
                s_tx_byte_count += item_size;
                vRingbufferReturnItem(s_tx_ringbuf, (void *)item);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

bool usb_cdc_init(void) {
    ESP_LOGI(TAG, "Initializing TinyUSB CDC-ACM stack...");

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TinyUSB driver: 0x%x", err);
        return false;
    }

    const tinyusb_config_cdcacm_t amc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = &tinyusb_cdc_line_state_callback,
        .callback_line_coding_changed = NULL
    };
    err = tinyusb_cdcacm_init(&amc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize TinyUSB CDC-ACM: 0x%x", err);
        return false;
    }

    // Allocate ring buffer for client -> host transmit queue
    s_tx_ringbuf = xRingbufferCreate(2048, RINGBUF_TYPE_BYTEBUF);
    if (s_tx_ringbuf == NULL) {
        ESP_LOGE(TAG, "Failed to create CDC TX ring buffer!");
        return false;
    }

    // Spawn polling task pinned to Core 1
    BaseType_t res = xTaskCreatePinnedToCore(
        tinyusb_task,
        "task_tinyusb",
        4096,
        NULL,
        PRIORITY_TASK_TINYUSB,
        NULL,
        CORE_HARDWARE_INGESTION
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to launch task_tinyusb on Core %d", CORE_HARDWARE_INGESTION);
        return false;
    }

    ESP_LOGI(TAG, "TinyUSB CDC-ACM initialized successfully");
    return true;
}

size_t usb_cdc_send_to_host(const uint8_t *data, size_t length) {
    if (s_tx_ringbuf == NULL || data == NULL || length == 0) {
        return 0;
    }

    BaseType_t ok = xRingbufferSend(s_tx_ringbuf, data, length, pdMS_TO_TICKS(50));
    return (ok == pdTRUE) ? length : 0;
}

bool usb_cdc_is_connected(void) {
    return s_host_connected && tud_cdc_n_connected(0);
}

uint32_t usb_cdc_get_rx_bytes(void) {
    return s_rx_byte_count;
}

uint32_t usb_cdc_get_tx_bytes(void) {
    return s_tx_byte_count;
}
