#include "ws_hid.h"
#include "executor.h"
#include "hid_usb.h"
#include "hid_ble.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "ws_hid";

static void send_mouse_move(int8_t dx, int8_t dy)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) {
        hid_usb_mouse_move(dx, dy);
    } else {
        hid_ble_mouse_move(dx, dy);
    }
}

static void send_mouse_down(uint8_t btn)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) {
        hid_usb_mouse_down(btn);
    } else {
        hid_ble_mouse_down(btn);
    }
}

static void send_mouse_up(uint8_t btn)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) {
        hid_usb_mouse_up(btn);
    } else {
        hid_ble_mouse_up(btn);
    }
}

static void send_mouse_scroll(int8_t v, int8_t h)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) {
        hid_usb_mouse_scroll(v, h);
    } else {
        hid_ble_mouse_scroll(v, h);
    }
}

static void send_keyboard_down(uint8_t keycode, uint8_t mods)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) {
        hid_usb_down(keycode, mods);
    } else {
        hid_ble_down(keycode, mods);
    }
}

static void send_keyboard_up(uint8_t keycode, uint8_t mods)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) {
        hid_usb_up(keycode, mods);
    } else {
        hid_ble_up(keycode, mods);
    }
}

static void send_keyboard_tap(uint8_t keycode, uint8_t mods)
{
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) {
        hid_usb_tap(keycode, mods);
    } else {
        hid_ble_tap(keycode, mods);
    }
}

static void process_binary_frame(const uint8_t *data, size_t len)
{
    if (len < 4) return;
    uint8_t opcode = data[0];

    switch (opcode) {
        case 0x01: { /* Mouse move/scroll/buttons: [0x01, buttons, dx, dy, wheel] */
            uint8_t buttons = data[1];
            int8_t dx = (int8_t)data[2];
            int8_t dy = (int8_t)data[3];
            int8_t wheel = (len >= 5) ? (int8_t)data[4] : 0;

            static uint8_t s_prev_buttons = 0;
            if (buttons != s_prev_buttons) {
                uint8_t pressed = buttons & ~s_prev_buttons;
                uint8_t released = s_prev_buttons & ~buttons;
                if (pressed)  send_mouse_down(pressed);
                if (released) send_mouse_up(released);
                s_prev_buttons = buttons;
            }

            if (dx != 0 || dy != 0) {
                send_mouse_move(dx, dy);
            }
            if (wheel != 0) {
                send_mouse_scroll(wheel, 0);
            }
            break;
        }

        case 0x02: { /* Keyboard event: [0x02, action(0=up,1=down,2=tap), mods, keycode] */
            uint8_t act = data[1];
            uint8_t mods = data[2];
            uint8_t code = data[3];
            if (act == 1)      send_keyboard_down(code, mods);
            else if (act == 0) send_keyboard_up(code, mods);
            else if (act == 2) send_keyboard_tap(code, mods);
            break;
        }

        case 0x03: { /* Consumer key tap: [0x03, code_lsb, code_msb] */
            uint16_t code = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
            if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) {
                hid_usb_consumer_tap(code);
            }
            break;
        }

        default:
            break;
    }
}

static void process_json_frame(const char *text)
{
    cJSON *j = cJSON_Parse(text);
    if (!j) return;

    const cJSON *jt = cJSON_GetObjectItem(j, "t");
    if (jt && cJSON_IsString(jt)) {
        const char *t = jt->valuestring;
        if (strcmp(t, "m_mv") == 0) {
            const cJSON *jdx = cJSON_GetObjectItem(j, "dx");
            const cJSON *jdy = cJSON_GetObjectItem(j, "dy");
            int dx = (jdx && cJSON_IsNumber(jdx)) ? jdx->valueint : 0;
            int dy = (jdy && cJSON_IsNumber(jdy)) ? jdy->valueint : 0;
            send_mouse_move((int8_t)dx, (int8_t)dy);
        } else if (strcmp(t, "m_btn") == 0) {
            const cJSON *jb = cJSON_GetObjectItem(j, "btn");
            const cJSON *jd = cJSON_GetObjectItem(j, "down");
            uint8_t btn = (jb && cJSON_IsNumber(jb)) ? (uint8_t)jb->valueint : 1;
            bool down = (jd && cJSON_IsBool(jd)) ? cJSON_IsTrue(jd) : false;
            if (down) send_mouse_down(btn);
            else      send_mouse_up(btn);
        } else if (strcmp(t, "m_wheel") == 0) {
            const cJSON *jv = cJSON_GetObjectItem(j, "v");
            const cJSON *jh = cJSON_GetObjectItem(j, "h");
            int v = (jv && cJSON_IsNumber(jv)) ? jv->valueint : 0;
            int h = (jh && cJSON_IsNumber(jh)) ? jh->valueint : 0;
            send_mouse_scroll((int8_t)v, (int8_t)h);
        } else if (strcmp(t, "k_event") == 0) {
            const cJSON *jc = cJSON_GetObjectItem(j, "code");
            const cJSON *jm = cJSON_GetObjectItem(j, "mods");
            const cJSON *jd = cJSON_GetObjectItem(j, "down");
            uint8_t code = (jc && cJSON_IsNumber(jc)) ? (uint8_t)jc->valueint : 0;
            uint8_t mods = (jm && cJSON_IsNumber(jm)) ? (uint8_t)jm->valueint : 0;
            bool down = (jd && cJSON_IsBool(jd)) ? cJSON_IsTrue(jd) : false;
            if (down) send_keyboard_down(code, mods);
            else      send_keyboard_up(code, mods);
        }
    } else {
        /* Generic action payload */
        char ec[8] = {0};
        char em[64] = {0};
        executor_run_payload_sync(j, ec, sizeof(ec), em, sizeof(em));
    }

    cJSON_Delete(j);
}

esp_err_t ws_hid_handle_frame(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket HID client connected");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    if (ws_pkt.len > 0 && ws_pkt.len < 2048) {
        uint8_t *buf = calloc(1, ws_pkt.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK) {
            if (ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
                process_binary_frame(ws_pkt.payload, ws_pkt.len);
            } else if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
                process_json_frame((const char *)ws_pkt.payload);
            }
        }
        free(buf);
    }
    return ESP_OK;
}
