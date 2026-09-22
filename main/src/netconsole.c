#include "netconsole.h"
#include "app_stream_buffer.h"
#include "app_config.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "netconsole";

static TaskHandle_t s_task_handle = NULL;
static int s_sock_fd = -1;
static uint32_t s_packets_received = 0;
static uint32_t s_bytes_received = 0;
static bool s_running = false;

static void netconsole_task(void *pvParameters) {
    uint16_t port = (uint16_t)(uintptr_t)pvParameters;
    if (port == 0) port = NETCONSOLE_UDP_PORT;

    ESP_LOGI(TAG, "Netconsole listener starting on UDP port %u on Core %d", port, xPortGetCoreID());

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    s_sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock_fd < 0) {
        ESP_LOGE(TAG, "Failed to create Netconsole UDP socket: errno %d", errno);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    // Set receive timeout
    struct timeval timeout = {
        .tv_sec = 1,
        .tv_usec = 0
    };
    setsockopt(s_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (bind(s_sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind Netconsole socket to port %u: errno %d", port, errno);
        close(s_sock_fd);
        s_sock_fd = -1;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Netconsole bound successfully to port %u", port);
    s_running = true;

    char rx_buf[1024];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);

    while (s_running) {
        int len = recvfrom(s_sock_fd, rx_buf, sizeof(rx_buf) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
        if (len > 0) {
            rx_buf[len] = '\0';
            s_packets_received++;
            s_bytes_received += len;

            // Ingest incoming kernel crash / log frame into the 10KB stream buffer
            stream_buffer_write_bytes((const uint8_t *)rx_buf, len);
        } else if (len < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
            ESP_LOGW(TAG, "recvfrom error: errno %d", errno);
        }
    }

    if (s_sock_fd >= 0) {
        close(s_sock_fd);
        s_sock_fd = -1;
    }
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

bool netconsole_init(uint16_t port) {
    if (s_task_handle != NULL) {
        ESP_LOGW(TAG, "Netconsole task is already running");
        return true;
    }

    s_running = true;
    BaseType_t res = xTaskCreatePinnedToCore(
        netconsole_task,
        "task_netconsole",
        4096,
        (void *)(uintptr_t)port,
        PRIORITY_TASK_NETCONSOLE,
        &s_task_handle,
        CORE_HARDWARE_INGESTION
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn task_netconsole on Core %d", CORE_HARDWARE_INGESTION);
        s_running = false;
        return false;
    }

    return true;
}

void netconsole_stop(void) {
    s_running = false;
}

uint32_t netconsole_get_packets_received(void) {
    return s_packets_received;
}

uint32_t netconsole_get_bytes_received(void) {
    return s_bytes_received;
}
