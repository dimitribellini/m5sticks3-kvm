#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "version.h"

// Project metadata
#define APP_NAME                    "M5StickS3 OOB KVM"
#define APP_BUILD_TIMESTAMP         __DATE__ " " __TIME__
#define NVS_NAMESPACE               "blackbox_oob"

// Hardware Pinout Definitions (M5Stack StickS3 / SKU: K150)
#define PIN_BUTTON_M5               11   // KEY1 (Front Button)
#define PIN_BUTTON_SIDE             12   // KEY2 (Side Button)
#define PIN_I2C_SDA                 47   // AXP/M5PM1 & IMU SDA
#define PIN_I2C_SCL                 48   // AXP/M5PM1 & IMU SCL
#define PIN_LCD_MOSI                39
#define PIN_LCD_SCK                 40
#define PIN_LCD_DC                  45
#define PIN_LCD_CS                  41
#define PIN_LCD_RST                 21
#define PIN_LCD_BL                  38

// Power Thresholds
#define VBUS_DROP_THRESHOLD_VOLTS   4.0f
#define PMIC_POLL_INTERVAL_MS       500

// Ingestion & Memory Buffers
#define STREAM_BUFFER_SIZE_BYTES    10240  // 10KB inter-core ingestion stream buffer
#define ROLLING_LOG_SIZE_BYTES      10240  // 10KB circular historical memory window
#define STREAM_CHUNK_READ_SIZE      512

// Network & Sockets
#define NETCONSOLE_UDP_PORT         6666
#define WOL_UDP_PORT                9
#define ZABBIX_DEFAULT_PORT         10051
#define HTTP_SERVER_PORT            80
#define WS_TERMINAL_PATH            "/ws/terminal"

// Default Provisioning & Fallback Credentials
#define DEFAULT_WIFI_SSID           ""
#define DEFAULT_WIFI_PASS           ""
#define DEFAULT_AP_SSID             "M5StickS3-KVM-Setup"
#define DEFAULT_AP_PASS             "password123"
#define DEFAULT_HTTP_USER           "admin"
#define DEFAULT_HTTP_PASS           "admin"
#define DEFAULT_ZABBIX_HOST         "kvm-oob"
#define DEFAULT_ZABBIX_ITEM_KEY     "system.oob.panic"

// Core Affinities
#define CORE_HARDWARE_INGESTION     1
#define CORE_NETWORK_LOGIC          0

// Task Priorities
#define PRIORITY_TASK_TINYUSB       (tskIDLE_PRIORITY + 5)
#define PRIORITY_TASK_NETCONSOLE    (tskIDLE_PRIORITY + 4)
#define PRIORITY_TASK_PMIC          (tskIDLE_PRIORITY + 3)
#define PRIORITY_TASK_STRING_MATCH  (tskIDLE_PRIORITY + 3)
#define PRIORITY_TASK_HTTPD         (tskIDLE_PRIORITY + 3)
#define PRIORITY_TASK_ZABBIX        (tskIDLE_PRIORITY + 2)
#define PRIORITY_TASK_TELEMETRY     (tskIDLE_PRIORITY + 1)

// Alert States
typedef enum {
    ALERT_STATE_OK = 0,
    ALERT_STATE_VBUS_DROP,
    ALERT_STATE_SERIAL_LOST,
    ALERT_STATE_OOM_DETECTED,
    ALERT_STATE_KERNEL_PANIC,
    ALERT_STATE_MAX
} alert_state_t;

const char* alert_state_to_str(alert_state_t state);

#ifdef __cplusplus
}
#endif
