#include "app_config.h"
#include "config_manager.h"
#include "app_stream_buffer.h"
#include "pmic.h"
#include "display.h"
#include "usb_cdc.h"
#include "netconsole.h"
#include "string_matcher.h"
#include "zabbix.h"
#include "wifi_manager.h"
#include "web_server.h"

#include <M5Unified.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_main";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "Starting %s v%s...", APP_NAME, APP_VERSION);
    ESP_LOGI(TAG, "Target: M5Stack StickS3 (ESP32-S3-PICO-1-N8R8)");
    ESP_LOGI(TAG, "Partition strategy: Monolithic (.rodata embedded)");
    ESP_LOGI(TAG, "NVS isolation namespace: '%s'", NVS_NAMESPACE);
    ESP_LOGI(TAG, "==================================================");

    // 1. Initialize M5Unified hardware abstraction
    auto cfg = M5.config();
    M5.begin(cfg);
    ESP_LOGI(TAG, "M5Unified initialized successfully");

    // 2. Initialize NVS Configuration Manager (isolated namespace "blackbox_oob")
    if (!config_manager_init()) {
        ESP_LOGE(TAG, "Configuration manager initialization failed!");
    }

    // 3. Initialize 10KB FreeRTOS StreamBuffer & circular historical log
    stream_buffer_subsystem_init();

    // 4. Launch PMIC supervisor on Core 1 (VBUS & Battery monitoring)
    pmic_supervisor_init();

    // 5. Launch Display UI & Button handling on Core 1
    display_subsystem_init();

    // 6. Initialize TinyUSB CDC-ACM Device on Core 1
    usb_cdc_init();

    // 7. Initialize Wi-Fi subsystem (Station with SoftAP fallback)
    wifi_manager_init();
    wifi_manager_start();

    // 8. Launch Netconsole UDP Listener on Core 1 (port 6666)
    const app_config_data_t *app_cfg = config_manager_get();
    netconsole_init(app_cfg->netconsole_port);

    // 9. Launch Kernel Panic / OOM String Matcher on Core 0
    string_matcher_init();

    // 10. Launch Zabbix Trapper TCP sender & alert worker on Core 0
    zabbix_subsystem_init();

    // 11. Launch Embedded Web & WebSocket Console Server on Core 0
    web_server_init();

    // 12. Check for any pending crash dumps saved to NVS during a previous blackout
    zabbix_check_and_drain_nvs_dump();

    ESP_LOGI(TAG, "All subsystems successfully launched and running across dual cores.");
}
