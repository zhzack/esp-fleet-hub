#include "executor.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "hid_ble.h"
#include "hid_usb.h"
#include "keymap.h"
#include "power_sw.h"
#include "ws_client.h"

static const char *TAG = "exec";
static QueueHandle_t s_q;
static executor_transport_t s_transport = (executor_transport_t)EXECUTOR_DEFAULT_TRANSPORT_MODE;

typedef struct {
    bool debug_no_hid;
    bool skip_link_check;
} exec_options_t;

static const char *transport_name(executor_transport_t t)
{
    switch (t) {
        case EXECUTOR_TRANSPORT_USB:  return "usb";
        case EXECUTOR_TRANSPORT_BLE:  return "ble";
        default:                      return "auto";
    }
}

void executor_set_transport(executor_transport_t t) { s_transport = t; }
executor_transport_t executor_get_transport(void)   { return s_transport; }

executor_transport_t executor_active_transport(void)
{
    if (s_transport == EXECUTOR_TRANSPORT_USB) return EXECUTOR_TRANSPORT_USB;
    if (s_transport == EXECUTOR_TRANSPORT_BLE) return EXECUTOR_TRANSPORT_BLE;
    return hid_usb_mounted() ? EXECUTOR_TRANSPORT_USB : EXECUTOR_TRANSPORT_BLE;
}

static void hid_tap(uint8_t code, uint8_t mods)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) hid_usb_tap(code, mods);
    else                                                       hid_ble_tap(code, mods);
}

static void hid_down(uint8_t code, uint8_t mods)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) hid_usb_down(code, mods);
    else                                                       hid_ble_down(code, mods);
}

static void hid_up(uint8_t code, uint8_t mods)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) hid_usb_up(code, mods);
    else                                                       hid_ble_up(code, mods);
}

static void hid_mouse_move(int8_t dx, int8_t dy)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) hid_usb_mouse_move(dx, dy);
    else                                                       hid_ble_mouse_move(dx, dy);
}

static void hid_mouse_click(uint8_t button)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) hid_usb_mouse_click(button);
    else                                                       hid_ble_mouse_click(button);
}

static void hid_mouse_down(uint8_t button)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) hid_usb_mouse_down(button);
    else                                                       hid_ble_mouse_down(button);
}

static void hid_mouse_up(uint8_t button)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) hid_usb_mouse_up(button);
    else                                                       hid_ble_mouse_up(button);
}

static void hid_mouse_scroll(int8_t vertical, int8_t horizontal)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) hid_usb_mouse_scroll(vertical, horizontal);
    else                                                       hid_ble_mouse_scroll(vertical, horizontal);
}

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

static bool json_bool_or(const cJSON *obj, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static exec_options_t resolve_exec_options(const cJSON *step, const exec_options_t *parent)
{
    exec_options_t opts = parent ? *parent : (exec_options_t){0};
    opts.debug_no_hid = json_bool_or(step, "debug_no_hid", opts.debug_no_hid);
    opts.debug_no_hid = json_bool_or(step, "dry_run", opts.debug_no_hid);
    opts.skip_link_check = json_bool_or(step, "skip_link_check", opts.skip_link_check);
    return opts;
}

static bool exec_step(const cJSON *step, const exec_options_t *parent,
                      char *err_code, char *err_msg);

int executor_run_payload_sync(const cJSON *payload, char *errcode, int codelen,
                              char *err, int errlen)
{
    if (!payload) {
        if (err && errlen) snprintf(err, errlen, "payload missing");
        if (errcode && codelen) snprintf(errcode, codelen, "E004");
        return 400;
    }
    char ec[8] = {0}, em[80] = {0};
    bool ok = exec_step(payload, NULL, ec, em);
    if (errcode && codelen) strlcpy(errcode, ec[0] ? ec : "", codelen);
    if (err && errlen)      strlcpy(err, em, errlen);
    return ok ? 0 : 500;
}

static bool transport_link_up(char *err_code, char *err_msg)
{
    executor_transport_t t = executor_active_transport();
    if (t == EXECUTOR_TRANSPORT_USB) {
        if (hid_usb_mounted()) return true;
        strcpy(err_code, "E007");
        strcpy(err_msg, "no USB host");
        return false;
    }
    if (hid_ble_connected()) return true;
    strcpy(err_code, "E007");
    strcpy(err_msg, "no BLE peer connected");
    return false;
}

static bool exec_step(const cJSON *step, const exec_options_t *parent,
                      char *err_code, char *err_msg)
{
    exec_options_t opts = resolve_exec_options(step, parent);

    const cJSON *jt = cJSON_GetObjectItem(step, "type");
    if (!cJSON_IsString(jt)) {
        strcpy(err_code, "E004");
        strcpy(err_msg, "step missing type");
        return false;
    }
    const char *type = jt->valuestring;
    const cJSON *jk = cJSON_GetObjectItem(step, "key");
    const char *key = cJSON_IsString(jk) ? jk->valuestring : NULL;

    bool needs_link = strcmp(type, "delay") != 0 && strcmp(type, "macro") != 0;
    if (needs_link && !opts.skip_link_check && !transport_link_up(err_code, err_msg)) {
        return false;
    }

    if (!strcmp(type, "keyboard_tap") && key) {
        uint8_t code = 0, mods = 0;
        if (!keymap_lookup(key, &code, &mods)) {
            strcpy(err_code, "E004");
            strcpy(err_msg, "unknown key");
            return false;
        }
        if (!opts.debug_no_hid) hid_tap(code, mods);
        return true;
    }

    if (!strcmp(type, "keyboard_down") && key) {
        uint8_t code = 0, mods = 0;
        if (!keymap_lookup(key, &code, &mods)) {
            strcpy(err_code, "E004");
            strcpy(err_msg, "unknown key");
            return false;
        }
        const cJSON *jm = cJSON_GetObjectItem(step, "modifiers");
        if (cJSON_IsNumber(jm)) mods |= (uint8_t)(jm->valueint & 0xFF);
        if (!opts.debug_no_hid) hid_down(code, mods);
        return true;
    }

    if (!strcmp(type, "keyboard_up") && key) {
        uint8_t code = 0, mods = 0;
        if (!keymap_lookup(key, &code, &mods)) {
            strcpy(err_code, "E004");
            strcpy(err_msg, "unknown key");
            return false;
        }
        /* 释放语义只取 keymap 自身：常规键 mods=0（保留仍按住的修饰键），
         * 修饰键 code=0+mods=位 → 清该位。payload 的 modifiers 是修饰键
         * 快照，OR 进来会误释放仍按住的修饰键（组合键失效根因） */
        if (!opts.debug_no_hid) hid_up(code, mods);
        return true;
    }

    if (!strcmp(type, "text")) {
        const cJSON *jc = cJSON_GetObjectItem(step, "content");
        if (!cJSON_IsString(jc)) jc = cJSON_GetObjectItem(step, "text");
        if (!cJSON_IsString(jc)) {
            strcpy(err_code, "E004");
            strcpy(err_msg, "text missing");
            return false;
        }
        const cJSON *ji = cJSON_GetObjectItem(step, "interval_ms");
        int interval = cJSON_IsNumber(ji) ? ji->valueint : 5;
        for (const char *p = jc->valuestring; *p; ++p) {
            uint8_t code = 0, mods = 0;
            if (!keymap_ascii(*p, &code, &mods)) continue;
            if (!opts.debug_no_hid) hid_tap(code, mods);
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

    if (!strcmp(type, "mouse_move")) {
        const cJSON *jx = cJSON_GetObjectItem(step, "x");
        const cJSON *jy = cJSON_GetObjectItem(step, "y");
        int dx = cJSON_IsNumber(jx) ? jx->valueint : 0;
        int dy = cJSON_IsNumber(jy) ? jy->valueint : 0;
        if (dx > 127) dx = 127;
        if (dx < -127) dx = -127;
        if (dy > 127) dy = 127;
        if (dy < -127) dy = -127;
        if (!opts.debug_no_hid) hid_mouse_move((int8_t)dx, (int8_t)dy);
        return true;
    }

    if (!strcmp(type, "mouse_click")) {
        const cJSON *jb = cJSON_GetObjectItem(step, "button");
        int b = cJSON_IsNumber(jb) ? jb->valueint : 1;
        if (!opts.debug_no_hid) hid_mouse_click((uint8_t)b);
        return true;
    }

    if (!strcmp(type, "mouse_down")) {
        const cJSON *jb = cJSON_GetObjectItem(step, "button");
        int b = cJSON_IsNumber(jb) ? jb->valueint : 1;
        if (!opts.debug_no_hid) hid_mouse_down((uint8_t)b);
        return true;
    }

    if (!strcmp(type, "mouse_up")) {
        const cJSON *jb = cJSON_GetObjectItem(step, "button");
        int b = cJSON_IsNumber(jb) ? jb->valueint : 1;
        if (!opts.debug_no_hid) hid_mouse_up((uint8_t)b);
        return true;
    }

    if (!strcmp(type, "mouse_scroll")) {
        const cJSON *jx = cJSON_GetObjectItem(step, "x");
        const cJSON *jy = cJSON_GetObjectItem(step, "y");
        int hx = cJSON_IsNumber(jx) ? jx->valueint : 0;
        int vy = cJSON_IsNumber(jy) ? jy->valueint : 0;
        if (hx > 127) hx = 127;
        if (hx < -127) hx = -127;
        if (vy > 127) vy = 127;
        if (vy < -127) vy = -127;
        if (!opts.debug_no_hid) hid_mouse_scroll((int8_t)vy, (int8_t)hx);
        return true;
    }

    if (!strcmp(type, "macro")) {
        const cJSON *steps = cJSON_GetObjectItem(step, "steps");
        if (!cJSON_IsArray(steps)) {
            strcpy(err_code, "E004");
            strcpy(err_msg, "macro missing steps");
            return false;
        }
        cJSON *sub;
        cJSON_ArrayForEach(sub, steps) {
            if (!exec_step(sub, &opts, err_code, err_msg)) return false;
        }
        return true;
    }

    if (!strcmp(type, "consumer") && key) {
        uint16_t code = 0;
        if (!keymap_consumer_lookup(key, &code)) {
            strcpy(err_code, "E004");
            strcpy(err_msg, "unknown consumer key");
            return false;
        }
        if (!opts.debug_no_hid) hid_usb_consumer_tap(code);
        return true;
    }

    if (!strcmp(type, "power_pulse")) {
        const cJSON *jd = cJSON_GetObjectItem(step, "duration_ms");
        if (!cJSON_IsNumber(jd)) jd = cJSON_GetObjectItem(step, "ms");
        int ms = cJSON_IsNumber(jd) ? jd->valueint : 500;
        power_sw_pulse(ms);
        return true;
    }

    strcpy(err_code, "E004");
    snprintf(err_msg, 64, "unsupported type: %s", type);
    return false;
}

static void exec_task(void *arg)
{
    (void)arg;
    action_msg_t *am;
    for (;;) {
        if (xQueueReceive(s_q, &am, portMAX_DELAY) != pdTRUE) continue;

        int64_t t0 = esp_timer_get_time();
        char err_code[8] = {0}, err_msg[80] = {0};
        cJSON *payload = cJSON_GetObjectItem(am->action.root, "payload");
        exec_options_t root_opts = payload ? resolve_exec_options(payload, NULL) : (exec_options_t){0};
        bool ok = payload ? exec_step(payload, &root_opts, err_code, err_msg) : false;
        int ms = (int)((esp_timer_get_time() - t0) / 1000);

        if (am->need_ack) {
            cJSON *env = protocol_make_ack(
                am->msg_id,
                ok,
                ms,
                ok ? NULL : (err_code[0] ? err_code : "E004"),
                ok ? NULL : err_msg
            );
            cJSON *pl = cJSON_GetObjectItem(env, "payload");
            const char *active = (executor_active_transport() == EXECUTOR_TRANSPORT_USB) ? "usb" : "ble";
            cJSON_AddStringToObject(pl, "transport", active);
            cJSON_AddBoolToObject(pl, "ble_connected", hid_ble_connected());
            cJSON_AddBoolToObject(pl, "usb_mounted", hid_usb_mounted());
            cJSON_AddBoolToObject(pl, "debug_no_hid", root_opts.debug_no_hid);
            cJSON_AddBoolToObject(pl, "skip_link_check", root_opts.skip_link_check);
            if (hid_ble_connected()) {
                cJSON_AddNumberToObject(pl, "peer_count", hid_ble_conn_count());
                hid_ble_peer_t cur;
                if (hid_ble_get_current_peer(&cur)) {
                    char a[20];
                    snprintf(a, sizeof a, "%02X:%02X:%02X:%02X:%02X:%02X",
                             cur.addr[5], cur.addr[4], cur.addr[3],
                             cur.addr[2], cur.addr[1], cur.addr[0]);
                    cJSON *peer = cJSON_AddObjectToObject(pl, "peer");
                    cJSON_AddStringToObject(peer, "addr", a);
                    cJSON_AddNumberToObject(peer, "addr_type", cur.addr_type);
                }
            }
            ws_client_send(env);
        }

        cJSON_Delete(am->action.root);
        free(am);
    }
}

esp_err_t executor_start(uint16_t queue_size)
{
    if (queue_size == 0) queue_size = 128;
    if (s_transport < EXECUTOR_TRANSPORT_AUTO || s_transport > EXECUTOR_TRANSPORT_USB) {
        ESP_LOGW(TAG, "invalid EXECUTOR_DEFAULT_TRANSPORT_MODE=%d, falling back to BLE",
                 (int)s_transport);
        s_transport = EXECUTOR_TRANSPORT_BLE;
    }
    s_q = xQueueCreate(queue_size, sizeof(action_msg_t *));
    if (!s_q) return ESP_ERR_NO_MEM;
    xTaskCreate(exec_task, "exec", 6144, NULL, 7, NULL);
    ESP_LOGI(TAG, "executor started (queue=%u, default_transport=%s)",
             queue_size, transport_name(s_transport));
    return ESP_OK;
}
