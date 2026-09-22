#include "display.h"
#include "app_config.h"
#include "pmic.h"
#include "usb_cdc.h"
#include "netconsole.h"
#include "wifi_manager.h"
#include "config_manager.h"
#include <M5Unified.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "display";

static uint8_t s_current_page = 0; // 0: Dashboard, 1: Setup/AP, 2: Diagnostics
static const uint8_t MAX_PAGES = 3;
static const uint32_t DISPLAY_SLEEP_TIMEOUT_MS = 60000; // 60 seconds
static uint32_t s_last_activity_time = 0;
static bool s_display_sleeping = false;
static alert_state_t s_last_alert_state = ALERT_STATE_OK;
static volatile bool s_force_redraw = true;

void display_set_page(uint8_t page) {
    if (page < MAX_PAGES) {
        s_current_page = page;
        s_last_activity_time = (uint32_t)(esp_timer_get_time() / 1000);
        s_force_redraw = true;
    }
}

void display_next_page(void) {
    s_current_page = (s_current_page + 1) % MAX_PAGES;
    s_last_activity_time = (uint32_t)(esp_timer_get_time() / 1000);
    s_force_redraw = true;
}

void display_wake(void) {
    if (s_display_sleeping) {
        M5.Display.wakeup();
        M5.Display.setBrightness(100);
        s_display_sleeping = false;
    }
    s_last_activity_time = (uint32_t)(esp_timer_get_time() / 1000);
    s_force_redraw = true;
}


static void format_bytes(char *out, size_t out_sz, uint32_t bytes) {
    if (bytes >= 1048576) {
        snprintf(out, out_sz, "%.1fMB", (float)bytes / 1048576.0f);
    } else if (bytes >= 1024) {
        snprintf(out, out_sz, "%.1fKB", (float)bytes / 1024.0f);
    } else {
        snprintf(out, out_sz, "%uB", (unsigned int)bytes);
    }
}

static void draw_header(M5Canvas *c) {
    alert_state_t alert = pmic_get_alert_state();
    bool usb_conn = usb_cdc_is_connected();
    bool is_alert = (alert != ALERT_STATE_OK || !usb_conn);
    bool blink_on = (((uint32_t)(esp_timer_get_time() / 500000)) % 2 == 0);

    // Header background bar with cyan or blinking red/orange alert border
    uint16_t header_bg = 0x0A26;
    if (alert == ALERT_STATE_VBUS_DROP || alert == ALERT_STATE_KERNEL_PANIC || alert == ALERT_STATE_OOM_DETECTED) {
        header_bg = blink_on ? TFT_RED : 0x0A26;
    } else if (alert == ALERT_STATE_SERIAL_LOST || !usb_conn) {
        header_bg = blink_on ? TFT_ORANGE : 0x0A26;
    }

    c->fillRect(0, 0, 240, 19, header_bg);
    c->drawFastHLine(0, 19, 240, is_alert ? (blink_on ? TFT_RED : TFT_ORANGE) : TFT_CYAN);

    c->setTextColor(TFT_WHITE, header_bg);
    c->setTextSize(1);
    if (is_alert && blink_on) {
        if (alert == ALERT_STATE_VBUS_DROP) {
            c->drawString("! POWER LOSS (VBUS) !", 6, 5);
        } else if (alert == ALERT_STATE_SERIAL_LOST || !usb_conn) {
            c->drawString("! SERIAL LOST (ACM0) !", 6, 5);
        } else if (alert == ALERT_STATE_KERNEL_PANIC) {
            c->drawString("! KERNEL PANIC !", 6, 5);
        } else {
            c->drawString("! ALARM ACTIVE !", 6, 5);
        }
    } else {
        c->drawString("M5StickS3 KVM v1.0", 6, 5);
    }

    // Battery status with charge indicator
    int32_t bat = pmic_get_battery_level();
    bool chg = pmic_is_charging();
    char bat_str[16];
    snprintf(bat_str, sizeof(bat_str), "%ld%%%s", (long)bat, chg ? "+" : "");

    uint16_t bat_color = (bat > 40) ? TFT_GREEN : ((bat > 15) ? TFT_YELLOW : TFT_RED);
    c->setTextColor(bat_color, header_bg);
    c->drawRightString(bat_str, 234, 5);
}

static void draw_page_dashboard(M5Canvas *c) {
    char ip_str[32] = {0};
    wifi_manager_get_ip_str(ip_str, sizeof(ip_str));
    bool connected = wifi_manager_is_connected();
    bool is_ap = wifi_manager_is_ap_mode();
    alert_state_t alert = pmic_get_alert_state();
    bool usb_conn = usb_cdc_is_connected();
    bool blink_on = (((uint32_t)(esp_timer_get_time() / 500000)) % 2 == 0);

    // Row 1: Host Serial Status
    c->setTextColor(TFT_LIGHTGRAY);
    c->drawString("Host USB:", 8, 24);
    if (usb_conn) {
        c->setTextColor(TFT_GREEN);
        c->drawString("Active (/dev/ttyACM0)", 72, 24);
    } else {
        c->setTextColor(blink_on ? TFT_RED : TFT_ORANGE);
        c->drawString("DISCONNECTED / CLOSED", 72, 24);
    }

    // Row 2: Network IP
    c->setTextColor(TFT_LIGHTGRAY);
    c->drawString("Network: ", 8, 42);
    c->setTextColor(is_ap ? TFT_YELLOW : (connected ? TFT_GREEN : TFT_RED));
    c->drawString(ip_str[0] ? ip_str : "Disconnected", 72, 42);

    // Row 3: Wi-Fi Mode / RSSI
    c->setTextColor(TFT_LIGHTGRAY);
    c->drawString("WiFi:    ", 8, 60);
    if (is_ap) {
        c->setTextColor(TFT_YELLOW);
        c->drawString("SoftAP Active", 72, 60);
    } else if (connected) {
        c->setTextColor(TFT_GREEN);
        char rssi_str[32];
        snprintf(rssi_str, sizeof(rssi_str), "Connected (%ddBm)", wifi_manager_get_rssi());
        c->drawString(rssi_str, 72, 60);
    } else {
        c->setTextColor(TFT_RED);
        c->drawString("Connecting...", 72, 60);
    }

    // Row 4: Alert Status
    c->setTextColor(TFT_LIGHTGRAY);
    c->drawString("Status:  ", 8, 78);
    uint16_t alert_color = TFT_GREEN;
    const char *status_label = "NORMAL (OK)";
    if (alert == ALERT_STATE_VBUS_DROP) {
        alert_color = blink_on ? TFT_RED : TFT_ORANGE;
        status_label = "VBUS DROP (<4.0V)";
    } else if (alert == ALERT_STATE_SERIAL_LOST || !usb_conn) {
        alert_color = blink_on ? TFT_RED : TFT_ORANGE;
        status_label = "SERIAL LOST (ACM0)";
    } else if (alert == ALERT_STATE_OOM_DETECTED) {
        alert_color = TFT_RED;
        status_label = "OOM DETECTED";
    } else if (alert == ALERT_STATE_KERNEL_PANIC) {
        alert_color = TFT_RED;
        status_label = "KERNEL PANIC";
    }
    c->setTextColor(alert_color);
    c->drawString(status_label, 72, 78);

    // Row 5: I/O Statistics
    c->setTextColor(TFT_LIGHTGRAY);
    c->drawString("Traffic: ", 8, 96);
    char rx_buf[16], tx_buf[16], traf_str[48];
    format_bytes(rx_buf, sizeof(rx_buf), usb_cdc_get_rx_bytes());
    format_bytes(tx_buf, sizeof(tx_buf), usb_cdc_get_tx_bytes());
    snprintf(traf_str, sizeof(traf_str), "RX: %s | TX: %s", rx_buf, tx_buf);
    c->setTextColor(TFT_WHITE);
    c->drawString(traf_str, 72, 96);

    // Footer Hint
    c->drawFastHLine(0, 118, 240, 0x18E3);
    c->setTextColor(TFT_DARKGRAY);
    c->drawCenterString("[M5: Page]  [Side: AP/Sleep]", 120, 122);
}

static void draw_page_setup(M5Canvas *c) {
    const app_config_data_t *cfg = config_manager_get();
    bool is_ap = wifi_manager_is_ap_mode();
    bool connected = wifi_manager_is_connected();
    char ip_str[32] = {0};
    wifi_manager_get_ip_str(ip_str, sizeof(ip_str));

    c->setTextColor(TFT_YELLOW);
    c->drawString("WIFI & NETWORK SETUP", 8, 24);

    c->setTextColor(TFT_WHITE);
    c->drawString("Mode:", 8, 44);
    c->setTextColor(is_ap ? TFT_YELLOW : TFT_GREEN);
    c->drawString(is_ap ? "SoftAP Active" : "Station Mode", 50, 44);

    c->setTextColor(TFT_WHITE);
    c->drawString("SSID:", 8, 64);
    c->setTextColor(TFT_CYAN);
    if (is_ap) {
        c->drawString(cfg->ap_ssid, 50, 64);
    } else {
        c->drawString(cfg->wifi_ssid[0] ? cfg->wifi_ssid : "(No SSID Saved)", 50, 64);
    }

    c->setTextColor(TFT_WHITE);
    c->drawString(is_ap ? "Pass:" : "State:", 8, 84);
    c->setTextColor(is_ap ? TFT_CYAN : (connected ? TFT_GREEN : TFT_ORANGE));
    if (is_ap) {
        c->drawString(cfg->ap_pass, 50, 84);
    } else {
        c->drawString(connected ? "Connected" : "Connecting...", 50, 84);
    }

    c->setTextColor(TFT_WHITE);
    c->drawString("URL: ", 8, 104);
    c->setTextColor(TFT_GREEN);
    char url_str[48];
    if (ip_str[0] != '\0') {
        snprintf(url_str, sizeof(url_str), "http://%s", ip_str);
    } else {
        snprintf(url_str, sizeof(url_str), "%s", is_ap ? "http://192.168.4.1" : "Waiting for IP...");
    }
    c->drawString(url_str, 50, 104);

    c->drawFastHLine(0, 118, 240, 0x18E3);
    c->setTextColor(TFT_DARKGRAY);
    c->drawCenterString("[M5: Page]  [Side: Toggle Mode]", 120, 122);
}

static void draw_page_diagnostics(M5Canvas *c) {
    c->setTextColor(TFT_CYAN);
    c->drawString("SYSTEM TELEMETRY", 8, 24);

    int64_t uptime_sec = esp_timer_get_time() / 1000000;
    char up_str[48];
    snprintf(up_str, sizeof(up_str), "Uptime: %02lld:%02lld:%02lld | NC: %u",
             uptime_sec / 3600, (uptime_sec % 3600) / 60, uptime_sec % 60,
             (unsigned int)netconsole_get_packets_received());
    c->setTextColor(TFT_WHITE);
    c->drawString(up_str, 8, 44);

    char pwr_str[48];
    snprintf(pwr_str, sizeof(pwr_str), "Power: VBUS %.2fV | Bat %ld%%%s",
             pmic_get_vbus_voltage(),
             (long)pmic_get_battery_level(),
             pmic_is_charging() ? "+" : "");
    c->drawString(pwr_str, 8, 64);

    char mem_str[64];
    snprintf(mem_str, sizeof(mem_str), "RAM: %uKB | PSRAM: %uKB",
             (unsigned int)(esp_get_free_heap_size() / 1024),
             (unsigned int)(esp_get_free_internal_heap_size() / 1024));
    c->drawString(mem_str, 8, 84);

    const app_config_data_t *cfg = config_manager_get();
    char zbx_str[96];
    snprintf(zbx_str, sizeof(zbx_str), "Zabbix: %s:%u", cfg->zabbix_server, cfg->zabbix_port);
    c->drawString(zbx_str, 8, 104);

    c->drawFastHLine(0, 118, 240, 0x18E3);
    c->setTextColor(TFT_DARKGRAY);
    c->drawCenterString("[M5: Page]  [Hold: Reboot]", 120, 122);
}

static void display_task(void *pvParameters) {
    ESP_LOGI(TAG, "Display task started on Core %d", xPortGetCoreID());

    // Rotate display to landscape (240x135)
    M5.Display.setRotation(1);
    M5.Display.setBrightness(100);

    // Initialize double-buffer sprite to eliminate all rendering artifacts
    M5Canvas canvas(&M5.Display);
    if (!canvas.createSprite(240, 135)) {
        ESP_LOGE(TAG, "Failed to allocate 240x135 sprite for double-buffering!");
    } else {
        ESP_LOGI(TAG, "Allocated 240x135 M5Canvas for artifact-free rendering");
    }

    s_last_activity_time = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t last_render_time = 0;
    s_force_redraw = true;

    while (1) {
        M5.update();
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        alert_state_t current_alert = pmic_get_alert_state();
        bool usb_conn = usb_cdc_is_connected();
        bool has_alarm = (current_alert != ALERT_STATE_OK || !usb_conn);

        if (has_alarm && s_last_alert_state == ALERT_STATE_OK) {
            // Power drop, serial loss, or kernel panic! Wake screen immediately and jump to dashboard
            if (s_display_sleeping) {
                M5.Display.wakeup();
                M5.Display.setBrightness(128);
                s_display_sleeping = false;
            }
            s_current_page = 0;
            s_last_activity_time = now;
            s_last_alert_state = current_alert;
            s_force_redraw = true;
        } else if (!has_alarm && s_last_alert_state != ALERT_STATE_OK) {
            s_last_alert_state = ALERT_STATE_OK;
            s_last_activity_time = now;
            s_force_redraw = true;
        }

        // Check Front Button (BtnA / KEY1 - GPIO11)
        if (M5.BtnA.wasClicked()) {
            if (s_display_sleeping) {
                M5.Display.wakeup();
                M5.Display.setBrightness(100);
                s_display_sleeping = false;
                s_last_activity_time = now;
                s_force_redraw = true;
            } else {
                display_next_page();
                // display_next_page() sets s_force_redraw = true
            }
        } else if (M5.BtnA.pressedFor(3000)) {
            ESP_LOGW(TAG, "Hardware override: Front button held 3s. Rebooting now...");
            canvas.fillScreen(TFT_RED);
            canvas.setTextColor(TFT_WHITE, TFT_RED);
            canvas.drawCenterString("REBOOTING...", 120, 60);
            canvas.pushSprite(0, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        // Check Side Button (BtnB / KEY2 - GPIO12)
        if (M5.BtnB.wasClicked()) {
            if (s_display_sleeping) {
                M5.Display.wakeup();
                M5.Display.setBrightness(100);
                s_display_sleeping = false;
                s_last_activity_time = now;
                s_force_redraw = true;
            } else {
                ESP_LOGI(TAG, "Side button clicked: Toggling Wi-Fi mode");
                wifi_manager_toggle_ap_mode();
                // If toggled to Station mode, switch to Page 0 (Dashboard) to see connection & IP
                // If toggled to SoftAP, switch to Page 1 (Setup) to see AP SSID/Pass
                s_current_page = wifi_manager_is_ap_mode() ? 1 : 0;
                s_last_activity_time = now;
                s_force_redraw = true;
            }
        } else if (M5.BtnB.wasDoubleClicked()) {
            ESP_LOGW(TAG, "Side button double-clicked: Shutting down device");
            pmic_power_off();
        }

        // Check inactivity timeout (60 seconds) to conserve battery (do not sleep if alarm active)
        if (!s_display_sleeping && (now - s_last_activity_time >= DISPLAY_SLEEP_TIMEOUT_MS) && !has_alarm) {
            ESP_LOGI(TAG, "Display inactivity timeout reached. Entering screen sleep.");
            M5.Display.setBrightness(0);
            M5.Display.sleep();
            s_display_sleeping = true;
        }

        if (s_display_sleeping) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        // Render clean frame if forced, on alarm animation, or every 500ms for telemetry/blink
        if (s_force_redraw || (now - last_render_time >= 500)) {
            s_force_redraw = false;
            last_render_time = now;

            canvas.fillScreen(TFT_BLACK);
            draw_header(&canvas);
            switch (s_current_page) {
                case 0: draw_page_dashboard(&canvas); break;
                case 1: draw_page_setup(&canvas); break;
                case 2: draw_page_diagnostics(&canvas); break;
                default: break;
            }
            canvas.pushSprite(0, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

}

bool display_subsystem_init(void) {
    BaseType_t res = xTaskCreatePinnedToCore(
        display_task,
        "task_display",
        4096,
        NULL,
        PRIORITY_TASK_TELEMETRY,
        NULL,
        CORE_HARDWARE_INGESTION
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to launch task_display on Core %d", CORE_HARDWARE_INGESTION);
        return false;
    }

    ESP_LOGI(TAG, "Display subsystem initialized with double-buffered canvas and auto-sleep");
    return true;
}
