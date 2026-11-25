#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "system_caps.h"
#include "log_capture.h"
#include "serial_cli.h"
#include "wifi_mgr.h"
#include "ota_mgr.h"
#include "web_server.h"
#include "fleet_agent.h"
#include "ws_gateway.h"

#if CONFIG_FEATURE_GPIO_CTRL
#include "mod_gpio.h"
#endif

#if CONFIG_FEATURE_CAMERA
#include "mod_camera.h"
#endif

#if CONFIG_FEATURE_PC_MONITOR
#include "mod_pc_monitor.h"
#endif

#if CONFIG_FEATURE_STORAGE
#include "mod_storage.h"
#endif

#if CONFIG_FEATURE_DISPLAY
#include "mod_display.h"
#endif

#if CONFIG_FEATURE_USB_OTG
#include "mod_usb_otg.h"
#endif

static const char *TAG = "unified_main";

void app_main(void)
{
    // 1. Immediately disable Brownout Detector to prevent reset during WiFi calibration
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    // 2. Initialize in-memory circular log buffer
    log_capture_init(16384);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  ESP Unified Framework v1.0.0 Starting...");
    ESP_LOGI(TAG, "==================================================");

    // 3. Initialize NVS flash storage
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 4. Initialize Core Configurations & Capabilities
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(system_caps_init());

    // 5. Initialize WiFi Networking & Services
    ESP_ERROR_CHECK(wifi_mgr_init());
    ESP_ERROR_CHECK(wifi_mgr_start());

    // 6. Initialize Dual-Partition OTA Manager
#if CONFIG_FEATURE_OTA
    ESP_ERROR_CHECK(ota_mgr_init());
#endif

    // 7. Initialize Conditional Feature Modules (Configured at Compile-Time)
#if CONFIG_FEATURE_GPIO_CTRL
    ESP_LOGI(TAG, "[Feature] Initializing GPIO Control Module...");
    mod_gpio_init();
#endif

#if CONFIG_FEATURE_CAMERA
    ESP_LOGI(TAG, "[Feature] Initializing Camera OV2640 Module...");
    mod_camera_init();
#endif

#if CONFIG_FEATURE_PC_MONITOR
    ESP_LOGI(TAG, "[Feature] Initializing Air101 ST7735 PC Monitor & Joystick...");
    mod_pc_monitor_init();
#endif

#if CONFIG_FEATURE_STORAGE
    ESP_LOGI(TAG, "[Feature] Initializing SPIFFS Storage Module...");
    mod_storage_init();
#endif

#if CONFIG_FEATURE_DISPLAY
    ESP_LOGI(TAG, "[Feature] Initializing Display Text/Status Module...");
    mod_display_init();
#endif

#if CONFIG_FEATURE_USB_OTG
    ESP_LOGI(TAG, "[Feature] Initializing USB-OTG Composite HID Module...");
    mod_usb_otg_init();
#endif

    // 8. Start Unified Web Platform Server
    ESP_ERROR_CHECK(web_server_start());

    // 9. Start Fleet Hub Registration & Sync Agent
#if CONFIG_FEATURE_FLEET_AGENT
    fleet_agent_init();
#endif

#if CONFIG_FEATURE_WS_GATEWAY
    ws_gateway_init();
#endif

    // 10. Start Lightweight Interactive Serial CLI
    serial_cli_init();

    ESP_LOGI(TAG, "ESP Unified Framework initialization complete.");

    // Background health monitoring loop
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        system_caps_t caps;
        system_caps_get(&caps);
        ESP_LOGI(TAG, "[Heartbeat] Uptime: %lus | Free Heap: %lu B | Free PSRAM: %lu B | WiFi RSSI: %d dBm",
                 (unsigned long)caps.uptime_sec,
                 (unsigned long)caps.free_heap,
                 (unsigned long)caps.free_psram,
                 caps.wifi_rssi);
    }
}
