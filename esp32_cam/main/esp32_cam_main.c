/*
 * ESP32-CAM Dedicated Wireless Camera Node
 * Features:
 *   - OV2640 HD camera driver with 4MB PSRAM frame buffer
 *   - Live MJPEG video stream (/stream) & single frame capture (/capture)
 *   - Responsive Web Control Center (Flashlight, Resolution, Live Stream)
 *   - Shared WiFi Manager, NVS Config, Ring Log Buffer (/api/logs)
 *   - Full Dual-Partition OTA Remote Firmware Upgrade (/api/ota/upload)
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "wifi_mgr.h"
#include "camera_driver.h"
#include "cam_web_server.h"
#include "esp_ota_ops.h"
#include "log_capture.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define FW_VERSION "esp32-cam-1.0"

static const char *TAG = "main";

static void wifi_keepalive_task(void *pv)
{
    char ip_buf[32] = {0};
    while (1) {
        if (!wifi_mgr_connected()) {
            ESP_LOGW(TAG, "WiFi not connected, retrying esp_wifi_connect()...");
            esp_wifi_connect();
        } else {
            if (wifi_mgr_get_ip(ip_buf, sizeof(ip_buf))) {
                ESP_LOGI(TAG, "==========================================================");
                ESP_LOGI(TAG, "🎉 ESP32-CAM is ONLINE!");
                ESP_LOGI(TAG, "   Web Portal & Live Stream: http://%s/", ip_buf);
                ESP_LOGI(TAG, "   Direct MJPEG Stream:     http://%s/stream", ip_buf);
                ESP_LOGI(TAG, "   Snapshot Capture:        http://%s/capture", ip_buf);
                ESP_LOGI(TAG, "==========================================================");
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    /* 0. 禁用硬件欠压检测 (Brownout Detector)，避免 WiFi 射频瞬间大电流触发复位 */
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    /* 1. 初始化 32KB 内存环形日志缓冲 */
    log_capture_init(32768);

    /* 2. 初始化 NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 3. 加载应用配置 */
    app_config_t cfg;
    ESP_ERROR_CHECK(app_config_load(&cfg));
    if (cfg.wifi_ssid[0] == '\0') {
        strlcpy(cfg.wifi_ssid, "1", sizeof cfg.wifi_ssid);
        strlcpy(cfg.wifi_pass, "11111111", sizeof cfg.wifi_pass);
    }
    ESP_LOGI(TAG, "Booting device_id=%s fw=%s target_ssid=%s", cfg.device_id, FW_VERSION, cfg.wifi_ssid);

    /* 4. 初始化 OV2640 摄像头 (启用 PSRAM 帧缓冲) */
    esp_err_t cam_err = camera_driver_init();
    if (cam_err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed, will still boot web portal for diagnostics/OTA");
    }

    /* 5. 连接本地 WiFi (等待 8 秒) */
    if (wifi_mgr_start(cfg.wifi_ssid, cfg.wifi_pass, 8000) != ESP_OK) {
        ESP_LOGW(TAG, "Initial WiFi connect timeout, starting AP fallback 'ESP32-CAM-LIVE' and background retry task...");
        wifi_mgr_ap_start("ESP32-CAM-LIVE");
    }

    /* 启动后台 WiFi 保持/重连任务 */
    xTaskCreate(wifi_keepalive_task, "wifi_keepalive", 4096, NULL, 5, NULL);

    /* 6. 取消 OTA 回滚保护，确认当前运行分区有效 */
    esp_ota_mark_app_valid_cancel_rollback();

    /* 7. 启动 mDNS 服务 (可通过 http://esp32-cam.local 直接访问) */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("esp32-cam");
        mdns_instance_name_set("ESP32-CAM Video Node");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS responder started: http://esp32-cam.local");
    }

    /* 8. 启动 Web 监控台与 MJPEG 实时推流服务端 */
    cam_web_server_start(cfg.device_id);

    char ip_buf[32] = {0};
    if (wifi_mgr_connected() && wifi_mgr_get_ip(ip_buf, sizeof(ip_buf))) {
        ESP_LOGI(TAG, "==========================================================");
        ESP_LOGI(TAG, "🎉 ESP32-CAM is READY!");
        ESP_LOGI(TAG, "   Web Portal & Live Stream: http://%s/", ip_buf);
        ESP_LOGI(TAG, "   Direct MJPEG Stream:     http://%s/stream", ip_buf);
        ESP_LOGI(TAG, "   Snapshot Capture:        http://%s/capture", ip_buf);
        ESP_LOGI(TAG, "==========================================================");
    }
}
