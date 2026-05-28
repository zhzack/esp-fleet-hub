#include "ws_client.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "protocol.h"

static const char *TAG = "ws";

static esp_websocket_client_handle_t s_client;
static QueueHandle_t s_tx_queue;          /* cJSON* envelopes pending send */
static volatile bool s_online = false;
static char s_device_id[64];

static void send_register(void);

bool ws_client_online(void) { return s_online; }

esp_err_t ws_client_send(cJSON *envelope)
{
    if (!envelope) return ESP_ERR_INVALID_ARG;
    if (xQueueSend(s_tx_queue, &envelope, 0) != pdTRUE) {
        ESP_LOGW(TAG, "tx queue full, dropping envelope");
        cJSON_Delete(envelope);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *e = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        send_register();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        s_online = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        if (e->op_code == 0x1 /* text */ && e->data_len > 0) {
            /* Note: large frames may arrive fragmented across multiple DATA events.
             * For MVP we expect frames to fit one event; protocol layer will reject
             * partials. */
            protocol_feed(e->data_ptr, e->data_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "error");
        s_online = false;
        break;
    default:
        break;
    }
}

static void send_register(void)
{
    cJSON *env = cJSON_CreateObject();
    cJSON *hdr = cJSON_AddObjectToObject(env, "header");
    char uuid[37];
    protocol_make_uuid(uuid);
    cJSON_AddStringToObject(hdr, "msg_id", uuid);
    cJSON_AddStringToObject(hdr, "type", "register");
    cJSON_AddStringToObject(hdr, "device_id", s_device_id);
    cJSON_AddNumberToObject(hdr, "timestamp", (double)time(NULL));
    cJSON_AddStringToObject(hdr, "source", "esp32");
    cJSON_AddObjectToObject(env, "routing");
    cJSON *pl = cJSON_AddObjectToObject(env, "payload");
    cJSON_AddStringToObject(pl, "device_id", s_device_id);
    cJSON_AddStringToObject(pl, "name", s_device_id);
    cJSON *caps = cJSON_AddArrayToObject(pl, "capabilities");
    cJSON_AddItemToArray(caps, cJSON_CreateString("ble_keyboard"));
    ws_client_send(env);
    s_online = true;  /* assume online; will be downgraded if register ack errors */
}

static void tx_task(void *arg)
{
    cJSON *env;
    for (;;) {
        if (xQueueReceive(s_tx_queue, &env, portMAX_DELAY) == pdTRUE) {
            char *s = cJSON_PrintUnformatted(env);
            if (s) {
                if (esp_websocket_client_is_connected(s_client)) {
                    esp_websocket_client_send_text(s_client, s, strlen(s), portMAX_DELAY);
                } else {
                    ESP_LOGW(TAG, "not connected, drop tx frame");
                }
                cJSON_free(s);
            }
            cJSON_Delete(env);
        }
    }
}

esp_err_t ws_client_start(const char *host, uint16_t port,
                          const char *device_id, const char *token)
{
    strlcpy(s_device_id, device_id, sizeof s_device_id);

    s_tx_queue = xQueueCreate(32, sizeof(cJSON *));
    if (!s_tx_queue) return ESP_ERR_NO_MEM;

    char uri[256];
    if (token && token[0]) {
        snprintf(uri, sizeof uri, "ws://%s:%u/ws/device?token=%s", host, port, token);
    } else {
        snprintf(uri, sizeof uri, "ws://%s:%u/ws/device", host, port);
    }
    ESP_LOGI(TAG, "connecting %s", uri);

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 0,    /* heartbeat handled at app layer */
        .disable_auto_reconnect = false,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                  on_ws_event, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_client));

    xTaskCreate(tx_task, "ws_tx", 4096, NULL, 6, NULL);
    return ESP_OK;
}
