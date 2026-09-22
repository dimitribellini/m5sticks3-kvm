#include "zabbix.h"
#include "app_stream_buffer.h"
#include "config_manager.h"
#include "app_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "zabbix";

typedef struct {
    alert_state_t alert_type;
    char message[128];
} zabbix_alert_msg_t;

static QueueHandle_t s_alert_queue = NULL;
static uint32_t s_alerts_sent = 0;
static uint32_t s_alerts_failed = 0;

static bool send_tcp_to_zabbix(const char *server_host, uint16_t port, const char *json_payload, size_t payload_len) {
    if (server_host == NULL || server_host[0] == '\0' || json_payload == NULL || payload_len == 0) {
        return false;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo *res = NULL;
    int err = getaddrinfo(server_host, port_str, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS resolution failed for Zabbix server: %s", server_host);
        return false;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create TCP socket: errno %d", errno);
        freeaddrinfo(res);
        return false;
    }

    // Set 3-second timeout for connect and send
    struct timeval timeout = {
        .tv_sec = 3,
        .tv_usec = 0
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "Failed to connect to Zabbix server %s:%u (errno %d)", server_host, port, errno);
        close(sock);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    // Zabbix Trapper protocol header:
    // "ZBXD\x01" (5 bytes) + uint64_t payload length in little-endian (8 bytes)
    uint8_t header[13];
    memcpy(header, "ZBXD\x01", 5);
    uint64_t len64 = payload_len;
    for (int i = 0; i < 8; i++) {
        header[5 + i] = (uint8_t)(len64 >> (i * 8));
    }

    if (send(sock, header, sizeof(header), 0) < 0 ||
        send(sock, json_payload, payload_len, 0) < 0) {
        ESP_LOGE(TAG, "Failed to send packet to Zabbix: errno %d", errno);
        close(sock);
        return false;
    }

    // Read response (acknowledgement)
    char rx_buf[128];
    int n = recv(sock, rx_buf, sizeof(rx_buf) - 1, 0);
    if (n > 0) {
        rx_buf[n] = '\0';
        ESP_LOGI(TAG, "Zabbix server response: %s", rx_buf);
    }

    close(sock);
    ESP_LOGI(TAG, "Successfully delivered alert to Zabbix server %s:%u", server_host, port);
    return true;
}

static size_t json_escape(const char *src, char *dest, size_t dest_max) {
    if (!src || !dest || dest_max == 0) return 0;
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d + 4 < dest_max; s++) {
        unsigned char c = (unsigned char)src[s];
        switch (c) {
            case '"':  dest[d++] = '\\'; dest[d++] = '"'; break;
            case '\\': dest[d++] = '\\'; dest[d++] = '\\'; break;
            case '\b': dest[d++] = '\\'; dest[d++] = 'b'; break;
            case '\f': dest[d++] = '\\'; dest[d++] = 'f'; break;
            case '\n': dest[d++] = '\\'; dest[d++] = 'n'; break;
            case '\r': dest[d++] = '\\'; dest[d++] = 'r'; break;
            case '\t': dest[d++] = '\\'; dest[d++] = 't'; break;
            default:
                if (c < 0x20) {
                    dest[d++] = ' ';
                } else {
                    dest[d++] = c;
                }
                break;
        }
    }
    dest[d] = '\0';
    return d;
}

static void process_alert(const zabbix_alert_msg_t *alert) {
    const app_config_data_t *cfg = config_manager_get();

    if (cfg->zabbix_server[0] == '\0') {
        ESP_LOGW(TAG, "Zabbix server not configured, skipping alert dispatch");
        return;
    }

    // Allocate memory for log snapshot, escaped logs, and JSON payload
    char *log_snapshot = (char *)malloc(ROLLING_LOG_SIZE_BYTES + 1);
    char *escaped_logs = (char *)malloc((ROLLING_LOG_SIZE_BYTES * 2) + 512);
    char *json_buf = (char *)malloc((ROLLING_LOG_SIZE_BYTES * 2) + 2048);

    if (!log_snapshot || !escaped_logs || !json_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for alert payload!");
        if (log_snapshot) free(log_snapshot);
        if (escaped_logs) free(escaped_logs);
        if (json_buf) free(json_buf);
        return;
    }

    rolling_log_get_snapshot(log_snapshot, ROLLING_LOG_SIZE_BYTES);
    json_escape(log_snapshot, escaped_logs, (ROLLING_LOG_SIZE_BYTES * 2) + 512);
    free(log_snapshot);

    char escaped_msg[256];
    json_escape(alert->message, escaped_msg, sizeof(escaped_msg));

    // Build RFC-compliant Zabbix Sender JSON payload
    int written = snprintf(
        json_buf, (ROLLING_LOG_SIZE_BYTES * 2) + 2048,
        "{\"request\":\"sender data\",\"data\":[{\"host\":\"%s\",\"key\":\"%s\",\"value\":\"ALERT: %s [%s] | Uptime: %llds\\n--- RECENT CONSOLE LOGS ---\\n%s\"}]}",
        cfg->zabbix_host,
        cfg->zabbix_item_key,
        alert_state_to_str(alert->alert_type),
        escaped_msg,
        (long long)(esp_timer_get_time() / 1000000),
        escaped_logs
    );

    free(escaped_logs);

    if (written > 0) {
        size_t payload_len = (size_t)written;
        bool sent = send_tcp_to_zabbix(cfg->zabbix_server, cfg->zabbix_port, json_buf, payload_len);

        if (sent) {
            s_alerts_sent++;
        } else {
            s_alerts_failed++;
            ESP_LOGW(TAG, "Zabbix unreachable! Committing emergency alert to NVS namespace 'blackbox_oob'...");
            config_manager_save_panic_dump(json_buf, payload_len);
        }
    }

    free(json_buf);
}

static void zabbix_task(void *pvParameters) {
    ESP_LOGI(TAG, "Zabbix TX task running on Core %d", xPortGetCoreID());

    zabbix_alert_msg_t alert;
    while (1) {
        if (xQueueReceive(s_alert_queue, &alert, portMAX_DELAY) == pdTRUE) {
            process_alert(&alert);
        }
    }
}

bool zabbix_subsystem_init(void) {
    if (s_alert_queue == NULL) {
        s_alert_queue = xQueueCreate(5, sizeof(zabbix_alert_msg_t));
        if (s_alert_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create alert queue!");
            return false;
        }
    }

    BaseType_t res = xTaskCreatePinnedToCore(
        zabbix_task,
        "task_zabbix",
        4096,
        NULL,
        PRIORITY_TASK_ZABBIX,
        NULL,
        CORE_NETWORK_LOGIC
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to launch task_zabbix on Core %d", CORE_NETWORK_LOGIC);
        return false;
    }

    ESP_LOGI(TAG, "Zabbix subsystem initialized");
    return true;
}

bool zabbix_trigger_alert(alert_state_t alert_type, const char *message) {
    if (s_alert_queue == NULL) return false;

    zabbix_alert_msg_t alert;
    alert.alert_type = alert_type;
    snprintf(alert.message, sizeof(alert.message), "%s", message ? message : "");

    BaseType_t res = xQueueSend(s_alert_queue, &alert, pdMS_TO_TICKS(100));
    return (res == pdTRUE);
}

void zabbix_check_and_drain_nvs_dump(void) {
    char *dump_buf = (char *)malloc(ROLLING_LOG_SIZE_BYTES + 2048);
    if (!dump_buf) return;

    size_t out_len = 0;
    if (config_manager_read_panic_dump(dump_buf, ROLLING_LOG_SIZE_BYTES + 2048, &out_len) && out_len > 0) {
        ESP_LOGI(TAG, "Found %u bytes pending panic dump in NVS. Transmitting to Zabbix...", (unsigned int)out_len);
        const app_config_data_t *cfg = config_manager_get();

        if (send_tcp_to_zabbix(cfg->zabbix_server, cfg->zabbix_port, dump_buf, out_len)) {
            ESP_LOGI(TAG, "Successfully drained NVS panic dump. Erasing NVS key.");
            config_manager_clear_panic_dump();
            s_alerts_sent++;
        } else {
            ESP_LOGW(TAG, "Retrying NVS panic dump transmission later.");
        }
    }

    free(dump_buf);
}

uint32_t zabbix_get_alerts_sent(void) {
    return s_alerts_sent;
}

uint32_t zabbix_get_alerts_failed(void) {
    return s_alerts_failed;
}
