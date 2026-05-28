#include "executor.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ws_client.h"
#include "hid_ble.h"
#include "keymap.h"

static const char *TAG = "exec";
static QueueHandle_t s_q;

uint16_t executor_queue_len(void)
{
    return s_q ? (uint16_t)uxQueueMessagesWaiting(s_q) : 0;
}

esp_err_t executor_enqueue(action_msg_t *am)
{
    if (!s_q) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_q, &am, 0) != pdTRUE) return ESP_FAIL;
    return ESP_OK;
}

static bool exec_step(const cJSON *step, char *err_code, char *err_msg);

int executor_run_payload_sync(const cJSON *payload, char *errcode, int codelen,
                              char *err, int errlen)
{
    if (!payload) {
        if (err && errlen) snprintf(err, errlen, "payload missing");
        if (errcode && codelen) snprintf(errcode, codelen, "E004");
        return 400;
    }
    char ec[8] = {0}, em[80] = {0};
    bool ok = exec_step(payload, ec, em);
    if (errcode && codelen) strlcpy(errcode, ec[0] ? ec : "", codelen);
    if (err && errlen)      strlcpy(err,     em,             errlen);
    return ok ? 0 : 500;
}

static bool exec_step(const cJSON *step, char *err_code, char *err_msg)
{
    const cJSON *jt = cJSON_GetObjectItem(step, "type");
    if (!cJSON_IsString(jt)) { strcpy(err_code, "E004"); strcpy(err_msg, "step missing type"); return false; }
    const char *type = jt->valuestring;
    const cJSON *jk = cJSON_GetObjectItem(step, "key");
    const char *key = cJSON_IsString(jk) ? jk->valuestring : NULL;

    if (!strcmp(type, "keyboard_tap") && key) {
        uint8_t code = 0, mods = 0;
        if (!keymap_lookup(key, &code, &mods)) { strcpy(err_code, "E004"); strcpy(err_msg, "unknown key"); return false; }
        hid_ble_tap(code, mods);
        return true;
    }
    if (!strcmp(type, "keyboard_down") && key) {
        uint8_t code = 0, mods = 0;
        if (!keymap_lookup(key, &code, &mods)) { strcpy(err_code, "E004"); strcpy(err_msg, "unknown key"); return false; }
        hid_ble_down(code, mods);
        return true;
    }
    if (!strcmp(type, "keyboard_up") && key) {
        uint8_t code = 0, mods = 0;
        if (!keymap_lookup(key, &code, &mods)) { strcpy(err_code, "E004"); strcpy(err_msg, "unknown key"); return false; }
        hid_ble_up(code, mods);
        return true;
    }
    if (!strcmp(type, "text")) {
        const cJSON *jc = cJSON_GetObjectItem(step, "content");
        if (!cJSON_IsString(jc)) jc = cJSON_GetObjectItem(step, "text");
        if (!cJSON_IsString(jc)) { strcpy(err_code, "E004"); strcpy(err_msg, "text missing"); return false; }
        const cJSON *ji = cJSON_GetObjectItem(step, "interval_ms");
        int interval = cJSON_IsNumber(ji) ? ji->valueint : 5;
        for (const char *p = jc->valuestring; *p; ++p) {
            uint8_t code = 0, mods = 0;
            if (!keymap_ascii(*p, &code, &mods)) continue;
            hid_ble_tap(code, mods);
            if (interval > 0) vTaskDelay(pdMS_TO_TICKS(interval));
        }
        return true;
    }
    if (!strcmp(type, "delay")) {
        const cJSON *jd = cJSON_GetObjectItem(step, "delay_ms");
        int ms = cJSON_IsNumber(jd) ? jd->valueint : 0;
        if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
        return true;
    }
    if (!strcmp(type, "mouse_move") || !strcmp(type, "mouse_click")) {
        /* BLE HID keyboard-only build for V1; mouse to be added with combined HID descriptor */
        strcpy(err_code, "E004");
        strcpy(err_msg, "mouse not supported on ble_hid build");
        return false;
    }
    if (!strcmp(type, "macro")) {
        const cJSON *steps = cJSON_GetObjectItem(step, "steps");
        if (!cJSON_IsArray(steps)) { strcpy(err_code, "E004"); strcpy(err_msg, "macro missing steps"); return false; }
        cJSON *sub;
        cJSON_ArrayForEach(sub, steps) {
            if (!exec_step(sub, err_code, err_msg)) return false;
        }
        return true;
    }
    strcpy(err_code, "E004");
    snprintf(err_msg, 64, "unsupported type: %s", type);
    return false;
}

static void exec_task(void *arg)
{
    action_msg_t *am;
    for (;;) {
        if (xQueueReceive(s_q, &am, portMAX_DELAY) != pdTRUE) continue;

        int64_t t0 = esp_timer_get_time();
        char err_code[8] = {0}, err_msg[80] = {0};
        cJSON *payload = cJSON_GetObjectItem(am->action.root, "payload");
        bool ok = payload ? exec_step(payload, err_code, err_msg) : false;
        int ms = (int)((esp_timer_get_time() - t0) / 1000);

        ws_client_send(ok
            ? protocol_make_ack(am->msg_id, true, ms, NULL, NULL)
            : protocol_make_ack(am->msg_id, false, ms, err_code[0] ? err_code : "E004", err_msg));

        cJSON_Delete(am->action.root);
        free(am);
    }
}

esp_err_t executor_start(uint16_t queue_size)
{
    if (queue_size == 0) queue_size = 128;
    s_q = xQueueCreate(queue_size, sizeof(action_msg_t *));
    if (!s_q) return ESP_ERR_NO_MEM;
    xTaskCreate(exec_task, "exec", 6144, NULL, 7, NULL);
    ESP_LOGI(TAG, "executor started (queue=%u)", queue_size);
    return ESP_OK;
}
