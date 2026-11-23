/*
 * ble_hid firmware entry point (Headless LAN Direct Controller).
 *
 * Boot order:
 *   1. Log capture buffer init (32KB RAM ring buffer)
 *   2. NVS init  → load config
 *   3. Start BLE HID (advertising) — host PC can pair while we configure WiFi
 *   4. Start TinyUSB HID (composite mouse + keyboard + consumer + CDC)
 *   5. Serial CLI start
 *   6. Hardware power pulse switch pin init
 *   7. If wifi_ssid is empty → run SoftAP provisioning portal, then reboot
 *   8. WiFi STA → connect to local AP
 *   9. Executor task start
 *  10. cmd_router init + OTA manager init
 *  11. LAN mode (REST API + WebSocket Streamer + mDNS announcement)
 *  12. Optional WebSocket client (if central gateway server is configured)
 */
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "wifi_mgr.h"
#include "ws_client.h"
#include "executor.h"
#include "heartbeat.h"
#include "hid_ble.h"
#include "hid_usb.h"
#include "provision.h"
#include "cmd_router.h"
#include "lan_mode.h"
#include "serial_cli.h"
#include "ota_mgr.h"
#include "power_sw.h"
#include "log_capture.h"

#define FW_VERSION "esp32-headless-2.3"

static const char *TAG = "main";

void app_main(void)
{
    /* 立即启用内存环形日志缓冲，以便通过局域网 HTTP 查看实时运行日志 */
    log_capture_init(32768);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_config_t cfg;
    ESP_ERROR_CHECK(app_config_load(&cfg));

    ESP_LOGI(TAG, "booting device_id=%s fw=%s", cfg.device_id, FW_VERSION);

    ESP_ERROR_CHECK(hid_ble_init(cfg.device_id));

    esp_err_t uerr = hid_usb_init(cfg.device_id);
    if (uerr == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "USB HID not supported on this target — BLE only");
    } else if (uerr != ESP_OK) {
        ESP_LOGW(TAG, "USB HID init failed: %s — BLE only", esp_err_to_name(uerr));
    }

    /* 启动交互式命令行（USB CDC 虚拟串口 / UART 终端） */
    serial_cli_start();

    /* 初始化预留硬件开关跳线 */
    power_sw_init(cfg.power_pin);

    if (cfg.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "no wifi configured — entering SoftAP provisioning");
        if (provision_run(0) == ESP_OK) {
            ESP_LOGI(TAG, "provisioned, rebooting...");
            esp_restart();
        } else {
            ESP_LOGE(TAG, "provisioning aborted; halting");
            return;
        }
    }

    if (wifi_mgr_start(cfg.wifi_ssid, cfg.wifi_pass, 30000) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed — BLE HID still available; will keep retrying WiFi internally");
    }

    ESP_ERROR_CHECK(executor_start(cfg.queue_size));

    cmd_router_init();
    ota_mgr_init();
    cmd_router_set_device_info(cfg.device_id, FW_VERSION);
    if (lan_mode_start(cfg.device_id) != ESP_OK) {
        ESP_LOGW(TAG, "lan_mode_start failed (non-fatal)");
    }

    if (cfg.server_host[0] != '\0' && cfg.device_token[0] != '\0') {
        ws_client_start(cfg.server_host, cfg.server_port, cfg.device_id, cfg.device_token);
        heartbeat_start(10);
    } else {
        ESP_LOGI(TAG, "standalone headless mode active: LAN REST + Direct WebSocket HID Stream");
    }

    ESP_LOGI(TAG, "app_main done — tasks running");
}
