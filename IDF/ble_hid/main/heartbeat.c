#include "heartbeat.h"

#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "app_config.h"
#include "executor.h"
#include "protocol.h"
#include "ws_client.h"
#include "wifi_mgr.h"
#include "hid_ble.h"

static const char *TAG = "hb";
static int64_t s_boot_us;

static void hb_task(void *arg)
{
    int period = (int)(intptr_t)arg;
    s_boot_us = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(period * 1000));
        if (!ws_client_online()) continue;

        cJSON *env = cJSON_CreateObject();
        cJSON *hdr = cJSON_AddObjectToObject(env, "header");
        char uuid[37]; protocol_make_uuid(uuid);
        cJSON_AddStringToObject(hdr, "msg_id", uuid);
        cJSON_AddStringToObject(hdr, "type", "heartbeat");
        cJSON_AddStringToObject(hdr, "device_id", app_config_get()->device_id);
        cJSON_AddNumberToObject(hdr, "timestamp", (double)time(NULL));
        cJSON_AddStringToObject(hdr, "source", "esp32");
        cJSON_AddObjectToObject(env, "routing");
        cJSON *pl = cJSON_AddObjectToObject(env, "payload");
        cJSON_AddNumberToObject(pl, "uptime", (double)((esp_timer_get_time() - s_boot_us) / 1000000));
        cJSON_AddNumberToObject(pl, "wifi_rssi", wifi_mgr_rssi());
        cJSON_AddNumberToObject(pl, "queue_len", executor_queue_len());
        cJSON_AddBoolToObject  (pl, "ble_connected", hid_ble_connected());
        ws_client_send(env);
        ESP_LOGD(TAG, "sent");
    }
}

esp_err_t heartbeat_start(int period_sec)
{
    if (period_sec < 1) period_sec = 10;
    xTaskCreate(hb_task, "hb", 3072, (void *)(intptr_t)period_sec, 3, NULL);
    return ESP_OK;
}
