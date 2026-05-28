/*
 * ble_hid firmware entry point.
 *
 * Boot order:
 *   1. NVS init  → load config
 *   2. Start BLE HID (advertising) — host PC can pair while we configure WiFi
 *   3. If wifi_ssid is empty → run SoftAP provisioning portal, then reboot
 *   4. WiFi STA → block until got IP
 *   5. cmd_router init + LAN mode (HTTP server + UDP discovery)
 *   6. WebSocket client → register with backend
 *   7. Executor + heartbeat tasks
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
#include "provision.h"
#include "cmd_router.h"
#include "lan_mode.h"

#define FW_VERSION "esp32-hid-1.1"

static const char *TAG = "main";

void app_main(void)
{
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
    cmd_router_set_device_info(cfg.device_id, FW_VERSION);
    if (lan_mode_start(cfg.device_id) != ESP_OK) {
        ESP_LOGW(TAG, "lan_mode_start failed (non-fatal)");
    }

    ESP_ERROR_CHECK(ws_client_start(cfg.server_host, cfg.server_port,
                                    cfg.device_id, cfg.device_token));
    ESP_ERROR_CHECK(heartbeat_start(10));

    ESP_LOGI(TAG, "app_main done — tasks running");
}
