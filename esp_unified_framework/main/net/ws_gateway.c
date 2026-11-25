#include "ws_gateway.h"
#include "sdkconfig.h"

#if CONFIG_FEATURE_WS_GATEWAY

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "cJSON.h"

#include "app_config.h"
#include "fleet_agent.h"
#include "wifi_mgr.h"
#if CONFIG_FEATURE_OTA
#include "ota_mgr.h"
#endif
#if CONFIG_FEATURE_USB_OTG
#include "mod_usb_otg.h"
#endif
#if CONFIG_FEATURE_GPIO_CTRL
#include "hal_gpio.h"
#endif
#if CONFIG_FEATURE_DISPLAY
#include "hal_display.h"
#endif
#if CONFIG_FEATURE_STORAGE
#include <dirent.h>
#include <sys/stat.h>
#include "hal_storage.h"
#endif
#if CONFIG_FEATURE_CAMERA
#include "hal_camera.h"
#endif

static const char *TAG = "ws_gw";

static esp_websocket_client_handle_t s_ws = NULL;
static QueueHandle_t s_rx_queue = NULL;
static volatile bool s_online = false;
static volatile bool s_connected = false;
static char s_auth_nonce[65] = "";

#define WS_RX_MAX_FRAME_LEN 16384
static char *s_rx_frag_buf = NULL;
static size_t s_rx_frag_cap = 0;
static size_t s_rx_frag_len = 0;

static void reset_rx_frag_buf(void)
{
    free(s_rx_frag_buf);
    s_rx_frag_buf = NULL;
    s_rx_frag_cap = 0;
    s_rx_frag_len = 0;
}

static void make_uuid(char out[37])
{
    uint8_t b[16];
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)(esp_random() & 0xFF);
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7], b[8],b[9],
             b[10],b[11],b[12],b[13],b[14],b[15]);
}

static void compute_hmac_hex(const char *secret, const char *nonce, char out_hex[65])
{
    uint8_t mac[32] = {0};
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info && mbedtls_md_hmac(info, (const unsigned char *)secret, strlen(secret),
                                (const unsigned char *)nonce, strlen(nonce), mac) == 0) {
        static const char hx[] = "0123456789abcdef";
        for (int i = 0; i < 32; i++) {
            out_hex[2 * i]     = hx[mac[i] >> 4];
            out_hex[2 * i + 1] = hx[mac[i] & 0x0F];
        }
        out_hex[64] = '\0';
    } else {
        out_hex[0] = '\0';
    }
}

static void ws_send_json_obj(cJSON *obj)
{
    if (!obj || !s_ws || !esp_websocket_client_is_connected(s_ws)) {
        if (obj) cJSON_Delete(obj);
        return;
    }
    char *txt = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (txt) {
        esp_websocket_client_send_text(s_ws, txt, strlen(txt), pdMS_TO_TICKS(1000));
        cJSON_free(txt);
    }
}

static void send_ack_with_payload(const char *ref_msg_id, bool ok, int exec_ms,
                                  cJSON *pl, const char *err_code, const char *err_msg)
{
    const app_config_t *cfg = app_config_ptr();
    cJSON *env = cJSON_CreateObject();
    cJSON *hdr = cJSON_AddObjectToObject(env, "header");
    char uuid[37];
    make_uuid(uuid);
    cJSON_AddStringToObject(hdr, "msg_id", uuid);
    cJSON_AddStringToObject(hdr, "type", "ack");
    cJSON_AddStringToObject(hdr, "device_id", cfg->device_id);
    cJSON_AddNumberToObject(hdr, "timestamp", (double)time(NULL));
    cJSON_AddStringToObject(hdr, "source", "esp32");
    if (ref_msg_id && ref_msg_id[0]) {
        cJSON_AddStringToObject(hdr, "ref_msg_id", ref_msg_id);
    }
    cJSON_AddObjectToObject(env, "routing");
    if (!pl) {
        pl = cJSON_CreateObject();
    }
    cJSON_AddItemToObject(env, "payload", pl);
    cJSON_AddBoolToObject(pl, "ok", ok);
    cJSON_AddStringToObject(pl, "status", ok ? "ok" : "failed");
    cJSON_AddNumberToObject(pl, "exec_time_ms", exec_ms);
    if (!ok) {
        cJSON *er = cJSON_AddObjectToObject(pl, "error");
        cJSON_AddStringToObject(er, "code", err_code ? err_code : "E500");
        cJSON_AddStringToObject(er, "message", err_msg ? err_msg : "failed");
    }
    ws_send_json_obj(env);
}

static void send_ack(const char *ref_msg_id, bool ok, int exec_ms, const char *err_code, const char *err_msg)
{
    send_ack_with_payload(ref_msg_id, ok, exec_ms, NULL, err_code, err_msg);
}

#if CONFIG_FEATURE_OTA
static void on_ws_ota_progress(const char *status, int pct, int bytes_read, int total_bytes, const char *err)
{
    if (!s_connected || !s_online) return;
    const app_config_t *cfg = app_config_ptr();
    cJSON *env = cJSON_CreateObject();
    cJSON *hdr = cJSON_AddObjectToObject(env, "header");
    char uuid[37];
    make_uuid(uuid);
    cJSON_AddStringToObject(hdr, "msg_id", uuid);
    cJSON_AddStringToObject(hdr, "type", "ota_progress");
    cJSON_AddStringToObject(hdr, "device_id", cfg->device_id);
    cJSON_AddNumberToObject(hdr, "timestamp", (double)time(NULL));
    cJSON_AddStringToObject(hdr, "source", "esp32");
    cJSON_AddObjectToObject(env, "routing");

    cJSON *pl = cJSON_AddObjectToObject(env, "payload");
    cJSON_AddStringToObject(pl, "status", status ? status : "downloading");
    cJSON_AddNumberToObject(pl, "progress", pct);
    cJSON_AddNumberToObject(pl, "bytes_read", bytes_read);
    cJSON_AddNumberToObject(pl, "total_bytes", total_bytes);
    if (err && err[0]) {
        cJSON_AddStringToObject(pl, "error", err);
    }
    ws_send_json_obj(env);
}
#endif

static void send_register(void)
{
    const app_config_t *cfg = app_config_ptr();
    cJSON *env = cJSON_CreateObject();
    cJSON *hdr = cJSON_AddObjectToObject(env, "header");
    char uuid[37];
    make_uuid(uuid);
    cJSON_AddStringToObject(hdr, "msg_id", uuid);
    cJSON_AddStringToObject(hdr, "type", "register");
    cJSON_AddStringToObject(hdr, "device_id", cfg->device_id);
    cJSON_AddNumberToObject(hdr, "timestamp", (double)time(NULL));
    cJSON_AddStringToObject(hdr, "source", "esp32");
    cJSON_AddObjectToObject(env, "routing");

    cJSON *pl = cJSON_AddObjectToObject(env, "payload");
    cJSON_AddStringToObject(pl, "device_id", cfg->device_id);
    cJSON_AddStringToObject(pl, "name", cfg->device_id);
    cJSON *caps = cJSON_AddArrayToObject(pl, "capabilities");
#if CONFIG_FEATURE_USB_OTG
    cJSON_AddItemToArray(caps, cJSON_CreateString("usb_keyboard"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("usb_mouse"));
#endif
#if CONFIG_FEATURE_GPIO_CTRL
    cJSON_AddItemToArray(caps, cJSON_CreateString("gpio"));
#endif
#if CONFIG_FEATURE_DISPLAY || CONFIG_FEATURE_PC_MONITOR
    cJSON_AddItemToArray(caps, cJSON_CreateString("display"));
#endif
#if CONFIG_FEATURE_CAMERA
    cJSON_AddItemToArray(caps, cJSON_CreateString("camera"));
#endif
#if CONFIG_FEATURE_OTA
    cJSON_AddItemToArray(caps, cJSON_CreateString("ota"));
#endif

    if (s_auth_nonce[0] && cfg->device_secret[0]) {
        char proof[65] = "";
        compute_hmac_hex(cfg->device_secret, s_auth_nonce, proof);
        cJSON *auth = cJSON_AddObjectToObject(pl, "auth");
        cJSON_AddStringToObject(auth, "nonce", s_auth_nonce);
        cJSON_AddStringToObject(auth, "proof", proof);
    }
    ws_send_json_obj(env);
}

static void send_heartbeat(void)
{
    const app_config_t *cfg = app_config_ptr();
    cJSON *env = cJSON_CreateObject();
    cJSON *hdr = cJSON_AddObjectToObject(env, "header");
    char uuid[37];
    make_uuid(uuid);
    cJSON_AddStringToObject(hdr, "msg_id", uuid);
    cJSON_AddStringToObject(hdr, "type", "heartbeat");
    cJSON_AddStringToObject(hdr, "device_id", cfg->device_id);
    cJSON_AddNumberToObject(hdr, "timestamp", (double)time(NULL));
    cJSON_AddStringToObject(hdr, "source", "esp32");
    cJSON_AddObjectToObject(env, "routing");
    cJSON_AddObjectToObject(env, "payload");
    ws_send_json_obj(env);
}

static bool exec_action_step(const cJSON *step, char *err_code, char *err_msg)
{
    const cJSON *jt = cJSON_GetObjectItem(step, "type");
    if (!cJSON_IsString(jt)) {
        strcpy(err_code, "E004");
        strcpy(err_msg, "missing action.type");
        return false;
    }
    const char *t = jt->valuestring;
    if (strcmp(t, "delay") == 0) {
        int ms = cJSON_IsNumber(cJSON_GetObjectItem(step, "delay_ms")) ? cJSON_GetObjectItem(step, "delay_ms")->valueint : 0;
        if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
        return true;
    }
    if (strcmp(t, "macro") == 0) {
        const cJSON *steps = cJSON_GetObjectItem(step, "steps");
        if (!cJSON_IsArray(steps)) return false;
        const cJSON *sub = NULL;
        cJSON_ArrayForEach(sub, steps) {
            if (!exec_action_step(sub, err_code, err_msg)) return false;
        }
        return true;
    }
#if CONFIG_FEATURE_USB_OTG
    if (!mod_usb_otg_is_mounted()) {
        strcpy(err_code, "E007");
        strcpy(err_msg, "USB host not mounted");
        return false;
    }
    if (strcmp(t, "mouse_move") == 0) {
        int dx = cJSON_IsNumber(cJSON_GetObjectItem(step, "x")) ? cJSON_GetObjectItem(step, "x")->valueint : 0;
        int dy = cJSON_IsNumber(cJSON_GetObjectItem(step, "y")) ? cJSON_GetObjectItem(step, "y")->valueint : 0;
        if (dx > 127) dx = 127;
        if (dx < -127) dx = -127;
        if (dy > 127) dy = 127;
        if (dy < -127) dy = -127;
        return mod_usb_otg_mouse_move((int8_t)dx, (int8_t)dy);
    }
    if (strcmp(t, "mouse_scroll") == 0) {
        int hx = cJSON_IsNumber(cJSON_GetObjectItem(step, "x")) ? cJSON_GetObjectItem(step, "x")->valueint : 0;
        int vy = cJSON_IsNumber(cJSON_GetObjectItem(step, "y")) ? cJSON_GetObjectItem(step, "y")->valueint : 0;
        if (hx > 127) hx = 127;
        if (hx < -127) hx = -127;
        if (vy > 127) vy = 127;
        if (vy < -127) vy = -127;
        return mod_usb_otg_mouse_scroll((int8_t)vy, (int8_t)hx);
    }
    if (strcmp(t, "mouse_click") == 0) {
        int b = cJSON_IsNumber(cJSON_GetObjectItem(step, "button")) ? cJSON_GetObjectItem(step, "button")->valueint : 1;
        return mod_usb_otg_mouse_click((uint8_t)b);
    }
    if (strcmp(t, "mouse_down") == 0) {
        int b = cJSON_IsNumber(cJSON_GetObjectItem(step, "button")) ? cJSON_GetObjectItem(step, "button")->valueint : 1;
        return mod_usb_otg_mouse_btn((uint8_t)b, true);
    }
    if (strcmp(t, "mouse_up") == 0) {
        int b = cJSON_IsNumber(cJSON_GetObjectItem(step, "button")) ? cJSON_GetObjectItem(step, "button")->valueint : 1;
        return mod_usb_otg_mouse_btn((uint8_t)b, false);
    }
    if (strcmp(t, "keyboard_tap") == 0) {
        const cJSON *jk = cJSON_GetObjectItem(step, "key");
        return cJSON_IsString(jk) && mod_usb_otg_key_tap(jk->valuestring, 0);
    }
    if (strcmp(t, "keyboard_down") == 0) {
        const cJSON *jk = cJSON_GetObjectItem(step, "key");
        const cJSON *jm = cJSON_GetObjectItem(step, "modifiers");
        return cJSON_IsString(jk) && mod_usb_otg_key_down(jk->valuestring, cJSON_IsNumber(jm) ? (uint8_t)jm->valueint : 0);
    }
    if (strcmp(t, "keyboard_up") == 0) {
        const cJSON *jk = cJSON_GetObjectItem(step, "key");
        return cJSON_IsString(jk) && mod_usb_otg_key_up(jk->valuestring);
    }
    if (strcmp(t, "text") == 0 || strcmp(t, "text_input") == 0) {
        const cJSON *jc = cJSON_GetObjectItem(step, "content");
        if (!cJSON_IsString(jc)) jc = cJSON_GetObjectItem(step, "text");
        int interval = cJSON_IsNumber(cJSON_GetObjectItem(step, "interval_ms")) ? cJSON_GetObjectItem(step, "interval_ms")->valueint : 5;
        return cJSON_IsString(jc) && mod_usb_otg_type_text(jc->valuestring, interval);
    }
#endif
    strcpy(err_code, "E004");
    snprintf(err_msg, 64, "unsupported action: %s", t);
    return false;
}

static void ws_worker_task(void *pv)
{
    char *frame = NULL;
    while (1) {
        if (xQueueReceive(s_rx_queue, &frame, portMAX_DELAY) != pdTRUE || !frame) continue;

        cJSON *root = cJSON_Parse(frame);
        free(frame);
        frame = NULL;
        if (!root) continue;

        const cJSON *hdr = cJSON_GetObjectItem(root, "header");
        const cJSON *pl  = cJSON_GetObjectItem(root, "payload");
        const char *mtype = cJSON_IsString(cJSON_GetObjectItem(hdr, "type")) ? cJSON_GetObjectItem(hdr, "type")->valuestring : "";
        const char *msgid = cJSON_IsString(cJSON_GetObjectItem(hdr, "msg_id")) ? cJSON_GetObjectItem(hdr, "msg_id")->valuestring : "";

        if (strcmp(mtype, "auth_challenge") == 0 && cJSON_IsObject(pl)) {
            const cJSON *jn = cJSON_GetObjectItem(pl, "nonce");
            if (cJSON_IsString(jn)) {
                strlcpy(s_auth_nonce, jn->valuestring, sizeof(s_auth_nonce));
                send_register();
            }
        } else if (strcmp(mtype, "ack") == 0) {
            const cJSON *jst = cJSON_GetObjectItem(pl, "status");
            if (cJSON_IsString(jst) && strcmp(jst->valuestring, "ok") == 0) {
                if (!s_online) {
                    s_online = true;
                    ESP_LOGI(TAG, "Registered with py_web HID Gateway (/ws/device)!");
                }
            }
        } else if (strcmp(mtype, "action") == 0 && cJSON_IsObject(pl)) {
            int64_t t0 = esp_timer_get_time();
            char ec[12] = "E500", em[64] = "exec failed";
            bool ok = exec_action_step(pl, ec, em);
            int ms = (int)((esp_timer_get_time() - t0) / 1000);
            const cJSON *na = cJSON_GetObjectItem(pl, "need_ack");
            const cJSON *lb = cJSON_GetObjectItem(pl, "loopback");
            const cJSON *at = cJSON_GetObjectItem(pl, "type");
            bool is_stream = cJSON_IsString(at) && (strcmp(at->valuestring, "mouse_move") == 0 || strcmp(at->valuestring, "mouse_scroll") == 0);
            if (!is_stream || cJSON_IsTrue(na) || cJSON_IsTrue(lb)) {
                send_ack(msgid, ok, ms, ec, em);
            }
        } else if (strcmp(mtype, "get_status") == 0) {
            char ip[32] = "", ssid[33] = "";
            wifi_mgr_get_ip(ip, sizeof(ip));
            wifi_mgr_get_ssid(ssid, sizeof(ssid));
            cJSON *st = cJSON_CreateObject();
            cJSON_AddStringToObject(st, "ip", ip);
            cJSON_AddStringToObject(st, "ssid", ssid);
            cJSON_AddNumberToObject(st, "rssi", wifi_mgr_rssi());
            cJSON_AddNumberToObject(st, "free_heap", (double)esp_get_free_heap_size());
            cJSON_AddNumberToObject(st, "uptime_sec", (double)(esp_timer_get_time() / 1000000ULL));
            cJSON_AddStringToObject(st, "fw_version", CONFIG_APP_FW_VERSION);
#if CONFIG_FEATURE_USB_OTG
            cJSON_AddBoolToObject(st, "usb_mounted", mod_usb_otg_is_mounted());
#endif
            send_ack_with_payload(msgid, true, 1, st, NULL, NULL);
        } else if (strcmp(mtype, "wifi.status") == 0) {
            char ip[32] = "", ssid[33] = "";
            wifi_mgr_get_ip(ip, sizeof(ip));
            wifi_mgr_get_ssid(ssid, sizeof(ssid));
            cJSON *st = cJSON_CreateObject();
            cJSON_AddBoolToObject(st, "connected", wifi_mgr_connected());
            cJSON_AddStringToObject(st, "ip", ip);
            cJSON_AddStringToObject(st, "ssid", ssid);
            cJSON_AddNumberToObject(st, "rssi", wifi_mgr_rssi());
            send_ack_with_payload(msgid, true, 1, st, NULL, NULL);
        } else if (strcmp(mtype, "wifi.scan") == 0) {
            int64_t t0 = esp_timer_get_time();
            char *sbuf = malloc(3072);
            if (!sbuf) {
                send_ack(msgid, false, 0, "E500", "oom");
            } else {
                wifi_mgr_scan(sbuf, 3072);
                cJSON *parsed = cJSON_Parse(sbuf);
                free(sbuf);
                int ms = (int)((esp_timer_get_time() - t0) / 1000);
                send_ack_with_payload(msgid, true, ms, parsed, NULL, NULL);
            }
        } else if (strcmp(mtype, "wifi.connect") == 0 && cJSON_IsObject(pl)) {
            const cJSON *js = cJSON_GetObjectItem(pl, "ssid");
            const cJSON *jp = cJSON_GetObjectItem(pl, "pass");
            if (!cJSON_IsString(jp)) jp = cJSON_GetObjectItem(pl, "password");
            if (!cJSON_IsString(js) || js->valuestring[0] == '\0') {
                send_ack(msgid, false, 0, "E400", "missing ssid");
            } else {
                esp_err_t werr = wifi_mgr_apply_new_sta(js->valuestring, cJSON_IsString(jp) ? jp->valuestring : "");
                if (werr == ESP_OK) {
                    send_ack(msgid, true, 100, NULL, NULL);
                } else {
                    send_ack(msgid, false, 100, "E500", esp_err_to_name(werr));
                }
            }
        } else if (strcmp(mtype, "usb.info") == 0) {
            cJSON *ui = cJSON_CreateObject();
#if CONFIG_FEATURE_USB_OTG
            cJSON_AddBoolToObject(ui, "enabled", true);
            cJSON_AddBoolToObject(ui, "mounted", mod_usb_otg_is_mounted());
#else
            cJSON_AddBoolToObject(ui, "enabled", false);
            cJSON_AddBoolToObject(ui, "mounted", false);
#endif
            send_ack_with_payload(msgid, true, 1, ui, NULL, NULL);
        } else if (strcmp(mtype, "config.set") == 0 && cJSON_IsObject(pl)) {
            char *ptxt = cJSON_PrintUnformatted(pl);
            esp_err_t cerr = ptxt ? app_config_update_from_json(ptxt) : ESP_ERR_NO_MEM;
            if (ptxt) cJSON_free(ptxt);
            send_ack(msgid, cerr == ESP_OK, 5, "E400", esp_err_to_name(cerr));
        } else if (strcmp(mtype, "ota.start") == 0 && cJSON_IsObject(pl)) {
#if CONFIG_FEATURE_OTA
            const cJSON *ju = cJSON_GetObjectItem(pl, "url");
            const cJSON *jh = cJSON_GetObjectItem(pl, "sha256");
            if (!cJSON_IsString(ju)) {
                send_ack(msgid, false, 0, "E400", "missing url");
            } else {
                ota_mgr_set_progress_cb(on_ws_ota_progress);
                esp_err_t oerr = ota_mgr_start_pull_async(ju->valuestring, cJSON_IsString(jh) ? jh->valuestring : NULL);
                send_ack(msgid, oerr == ESP_OK, 5, "E500", esp_err_to_name(oerr));
            }
#else
            send_ack(msgid, false, 0, "E404", "ota disabled");
#endif
        } else if (strcmp(mtype, "ota.status") == 0) {
#if CONFIG_FEATURE_OTA
            char obuf[512] = "";
            ota_mgr_status_to_json(obuf, sizeof(obuf));
            cJSON *op = cJSON_Parse(obuf);
            send_ack_with_payload(msgid, true, 1, op, NULL, NULL);
#else
            send_ack(msgid, false, 0, "E404", "ota disabled");
#endif
        } else if (strcmp(mtype, "reboot") == 0) {
            send_ack(msgid, true, 1, NULL, NULL);
            vTaskDelay(pdMS_TO_TICKS(400));
            esp_restart();
        } else if (strcmp(mtype, "sys.config") == 0) {
            char *cbuf = malloc(1536);
            if (!cbuf) {
                send_ack(msgid, false, 0, "E500", "oom");
            } else {
                app_config_to_json(cbuf, 1536);
                cJSON *parsed = cJSON_Parse(cbuf);
                free(cbuf);
                send_ack_with_payload(msgid, true, 1, parsed, NULL, NULL);
            }
        } else if (strcmp(mtype, "gpio.pins") == 0) {
#if CONFIG_FEATURE_GPIO_CTRL
            char *gbuf = malloc(1536);
            if (!gbuf) {
                send_ack(msgid, false, 0, "E500", "oom");
            } else {
                hal_gpio_pins_to_json(gbuf, 1536);
                cJSON *parsed = cJSON_Parse(gbuf);
                free(gbuf);
                send_ack_with_payload(msgid, true, 1, parsed, NULL, NULL);
            }
#else
            send_ack(msgid, false, 0, "E404", "gpio disabled");
#endif
        } else if ((strcmp(mtype, "gpio_set") == 0 || strcmp(mtype, "gpio.set") == 0) && cJSON_IsObject(pl)) {
#if CONFIG_FEATURE_GPIO_CTRL
            const cJSON *jp = cJSON_GetObjectItem(pl, "pin");
            const cJSON *jv = cJSON_GetObjectItem(pl, "val");
            const cJSON *jd = cJSON_GetObjectItem(pl, "duty");
            bool ok = false;
            if (cJSON_IsNumber(jp)) {
                if (cJSON_IsNumber(jd)) {
                    ok = (hal_gpio_set_pwm(jp->valueint, (uint8_t)jd->valueint) == ESP_OK);
                } else if (cJSON_IsNumber(jv)) {
                    ok = (hal_gpio_write(jp->valueint, jv->valueint) == ESP_OK);
                }
            }
            send_ack(msgid, ok, 1, "E500", "gpio failed");
#else
            send_ack(msgid, false, 0, "E404", "gpio disabled");
#endif
        } else if (strcmp(mtype, "display.status") == 0) {
#if CONFIG_FEATURE_DISPLAY
            cJSON *ds = cJSON_CreateObject();
            cJSON_AddBoolToObject(ds, "ready", hal_display_is_ready());
            cJSON_AddStringToObject(ds, "driver", hal_display_driver_name());
            cJSON_AddNumberToObject(ds, "backlight", hal_display_get_backlight());
            cJSON *lines = cJSON_AddArrayToObject(ds, "lines");
            for (int i = 0; i < HAL_DISPLAY_MAX_LINES; i++) {
                cJSON_AddItemToArray(lines, cJSON_CreateString(hal_display_get_line(i)));
            }
            send_ack_with_payload(msgid, true, 1, ds, NULL, NULL);
#else
            send_ack(msgid, false, 0, "E404", "display disabled");
#endif
        } else if (strcmp(mtype, "display.text") == 0 && cJSON_IsObject(pl)) {
#if CONFIG_FEATURE_DISPLAY
            if (cJSON_IsTrue(cJSON_GetObjectItem(pl, "clear"))) {
                hal_display_clear();
            }
            const cJSON *bl = cJSON_GetObjectItem(pl, "backlight");
            if (cJSON_IsNumber(bl)) {
                hal_display_set_backlight(bl->valueint);
            }
            for (int i = 0; i < HAL_DISPLAY_MAX_LINES; i++) {
                char k[8];
                snprintf(k, sizeof(k), "line%d", i);
                const cJSON *item = cJSON_GetObjectItem(pl, k);
                if (cJSON_IsString(item)) {
                    hal_display_set_line(i, item->valuestring);
                }
            }
            const cJSON *txt = cJSON_GetObjectItem(pl, "text");
            if (cJSON_IsString(txt)) {
                hal_display_set_line(0, txt->valuestring);
            }
            send_ack(msgid, true, 2, NULL, NULL);
#else
            send_ack(msgid, false, 0, "E404", "display disabled");
#endif
        } else if (strcmp(mtype, "storage.list") == 0) {
#if CONFIG_FEATURE_STORAGE
            size_t total = 0, used = 0;
            bool mounted = hal_storage_is_mounted();
            if (mounted) hal_storage_get_usage(&total, &used);
            cJSON *sr = cJSON_CreateObject();
            cJSON_AddBoolToObject(sr, "mounted", mounted);
            cJSON_AddNumberToObject(sr, "total_bytes", (double)total);
            cJSON_AddNumberToObject(sr, "used_bytes", (double)used);
            cJSON *files = cJSON_AddArrayToObject(sr, "files");
            if (mounted) {
                DIR *dir = opendir(hal_storage_base_path());
                if (dir) {
                    struct dirent *ent;
                    while ((ent = readdir(dir)) != NULL) {
                        char full[96];
                        struct stat st = {0};
                        snprintf(full, sizeof(full), "%s/%.64s", hal_storage_base_path(), ent->d_name);
                        if (stat(full, &st) == 0) {
                            cJSON *f = cJSON_CreateObject();
                            cJSON_AddStringToObject(f, "name", ent->d_name);
                            cJSON_AddNumberToObject(f, "size", (double)st.st_size);
                            cJSON_AddItemToArray(files, f);
                        }
                    }
                    closedir(dir);
                }
            }
            send_ack_with_payload(msgid, true, 2, sr, NULL, NULL);
#else
            send_ack(msgid, false, 0, "E404", "storage disabled");
#endif
        } else if (strcmp(mtype, "camera.flash") == 0 && cJSON_IsObject(pl)) {
#if CONFIG_FEATURE_CAMERA
            const cJSON *jd = cJSON_GetObjectItem(pl, "duty");
            int duty = cJSON_IsNumber(jd) ? jd->valueint : 0;
            if (duty < 0) duty = 0;
            if (duty > 255) duty = 255;
            esp_err_t ferr = hal_camera_set_flash_duty((uint8_t)duty);
            cJSON *fr = cJSON_CreateObject();
            cJSON_AddNumberToObject(fr, "flash_duty", duty);
            send_ack_with_payload(msgid, ferr == ESP_OK, 1, fr, "E500", esp_err_to_name(ferr));
#else
            send_ack(msgid, false, 0, "E404", "camera disabled");
#endif
        } else if (strcmp(mtype, "camera.config.get") == 0) {
#if CONFIG_FEATURE_CAMERA
            app_config_t cfg;
            app_config_get(&cfg);
            cJSON *cc = cJSON_CreateObject();
            cJSON_AddNumberToObject(cc, "framesize", cfg.cam_framesize);
            cJSON_AddNumberToObject(cc, "quality", cfg.cam_quality);
            cJSON_AddBoolToObject(cc, "vflip", cfg.cam_vflip);
            cJSON_AddBoolToObject(cc, "hmirror", cfg.cam_hmirror);
            cJSON_AddNumberToObject(cc, "brightness", cfg.cam_brightness);
            send_ack_with_payload(msgid, true, 1, cc, NULL, NULL);
#else
            send_ack(msgid, false, 0, "E404", "camera disabled");
#endif
        } else if (strcmp(mtype, "camera.config") == 0 && cJSON_IsObject(pl)) {
#if CONFIG_FEATURE_CAMERA
            app_config_t cfg;
            app_config_get(&cfg);
            const cJSON *fs = cJSON_GetObjectItem(pl, "framesize");
            const cJSON *q  = cJSON_GetObjectItem(pl, "quality");
            const cJSON *vf = cJSON_GetObjectItem(pl, "vflip");
            const cJSON *hm = cJSON_GetObjectItem(pl, "hmirror");
            const cJSON *br = cJSON_GetObjectItem(pl, "brightness");
            if (cJSON_IsNumber(fs)) cfg.cam_framesize = (uint8_t)fs->valueint;
            if (cJSON_IsNumber(q) && q->valueint >= 4 && q->valueint <= 63) cfg.cam_quality = (uint8_t)q->valueint;
            if (cJSON_IsBool(vf)) cfg.cam_vflip = cJSON_IsTrue(vf);
            if (cJSON_IsBool(hm)) cfg.cam_hmirror = cJSON_IsTrue(hm);
            if (cJSON_IsNumber(br)) cfg.cam_brightness = (int8_t)br->valueint;
            cfg.config_ver++;
            app_config_save(&cfg);
            esp_err_t cerr = hal_camera_apply_config(cfg.cam_framesize, cfg.cam_quality, cfg.cam_vflip, cfg.cam_hmirror, cfg.cam_brightness);
            send_ack(msgid, cerr == ESP_OK, 2, "E500", esp_err_to_name(cerr));
#else
            send_ack(msgid, false, 0, "E404", "camera disabled");
#endif
        } else if (strcmp(mtype, "camera.capture") == 0) {
#if CONFIG_FEATURE_CAMERA
            if (!hal_camera_is_ready()) {
                send_ack(msgid, false, 0, "E503", "camera not ready");
            } else {
                camera_fb_t *fb = hal_camera_fb_get();
                if (!fb) {
                    send_ack(msgid, false, 0, "E500", "frame grab failed");
                } else if (fb->len > 65536) {
                    hal_camera_fb_return(fb);
                    send_ack(msgid, false, 0, "E413", "frame too large for WS tunnel");
                } else {
                    size_t b64_cap = ((fb->len + 2) / 3) * 4 + 4;
                    unsigned char *b64 = malloc(b64_cap);
                    size_t olen = 0;
                    if (!b64 || mbedtls_base64_encode(b64, b64_cap, &olen, fb->buf, fb->len) != 0) {
                        free(b64);
                        hal_camera_fb_return(fb);
                        send_ack(msgid, false, 0, "E500", "base64 encode failed");
                    } else {
                        b64[olen] = '\0';
                        hal_camera_fb_return(fb);
                        cJSON *cap = cJSON_CreateObject();
                        cJSON_AddStringToObject(cap, "content_type", "image/jpeg");
                        cJSON_AddStringToObject(cap, "image_b64", (const char *)b64);
                        free(b64);
                        send_ack_with_payload(msgid, true, 5, cap, NULL, NULL);
                    }
                }
            }
#else
            send_ack(msgid, false, 0, "E404", "camera disabled");
#endif
        }

        cJSON_Delete(root);
    }
}

static void on_ws_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            s_connected = true;
            s_auth_nonce[0] = '\0';
            reset_rx_frag_buf();
            send_register();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_ERROR:
            s_connected = false;
            s_online = false;
            reset_rx_frag_buf();
            break;
        case WEBSOCKET_EVENT_DATA:
            if (e && (e->op_code == 0x1 || e->op_code == 0x0) && e->data_len > 0 && s_rx_queue) {
                if (e->payload_offset == 0 && e->data_len == e->payload_len && e->fin && e->data_len < WS_RX_MAX_FRAME_LEN) {
                    reset_rx_frag_buf();
                    char *copy = malloc(e->data_len + 1);
                    if (copy) {
                        memcpy(copy, e->data_ptr, e->data_len);
                        copy[e->data_len] = '\0';
                        if (xQueueSend(s_rx_queue, &copy, 0) != pdTRUE) free(copy);
                    }
                } else {
                    size_t total = (size_t)e->payload_len;
                    size_t end_off = (size_t)e->payload_offset + (size_t)e->data_len;
                    if (total == 0 || total >= WS_RX_MAX_FRAME_LEN || end_off >= WS_RX_MAX_FRAME_LEN) {
                        reset_rx_frag_buf();
                        break;
                    }
                    if (e->payload_offset == 0 || !s_rx_frag_buf || s_rx_frag_cap <= total) {
                        reset_rx_frag_buf();
                        s_rx_frag_buf = malloc(total + 1);
                        if (!s_rx_frag_buf) break;
                        s_rx_frag_cap = total + 1;
                    }
                    if ((size_t)e->payload_offset <= s_rx_frag_len && end_off <= total) {
                        memcpy(s_rx_frag_buf + e->payload_offset, e->data_ptr, e->data_len);
                        s_rx_frag_len = end_off;
                        if (s_rx_frag_len >= total && e->fin) {
                            s_rx_frag_buf[total] = '\0';
                            char *done_msg = s_rx_frag_buf;
                            s_rx_frag_buf = NULL;
                            s_rx_frag_cap = 0;
                            s_rx_frag_len = 0;
                            if (xQueueSend(s_rx_queue, &done_msg, 0) != pdTRUE) free(done_msg);
                        }
                    } else {
                        reset_rx_frag_buf();
                    }
                }
            }
            break;
        default:
            break;
    }
}

static bool resolve_ws_url(char *dst, size_t max_len)
{
    const app_config_t *cfg = app_config_ptr();
    if (cfg->ws_gateway_url[0] != '\0') {
        strlcpy(dst, cfg->ws_gateway_url, max_len);
        return true;
    }
    char hub[128] = "";
    if (cfg->hub_url[0] != '\0') {
        strlcpy(hub, cfg->hub_url, sizeof(hub));
    } else {
        fleet_agent_get_hub_url(hub, sizeof(hub));
    }
    if (strncmp(hub, "http://", 7) == 0) {
        snprintf(dst, max_len, "ws://%s/ws/device", hub + 7);
        return true;
    }
    if (strncmp(hub, "https://", 8) == 0) {
        snprintf(dst, max_len, "wss://%s/ws/device", hub + 8);
        return true;
    }
    return false;
}

static void ws_mgr_task(void *pv)
{
    char url[160] = "";
    char active_url[160] = "";
    vTaskDelay(pdMS_TO_TICKS(3000));

    while (1) {
        if (!wifi_mgr_connected()) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        if (!resolve_ws_url(url, sizeof(url))) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (!s_ws || strcmp(url, active_url) != 0) {
            if (s_ws) {
                esp_websocket_client_stop(s_ws);
                esp_websocket_client_destroy(s_ws);
                s_ws = NULL;
            }
            strlcpy(active_url, url, sizeof(active_url));
            esp_websocket_client_config_t wcfg = {
                .uri = active_url,
                .reconnect_timeout_ms = 5000,
                .network_timeout_ms = 8000,
                .task_stack = 6144,
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
                .crt_bundle_attach = esp_crt_bundle_attach,
#endif
            };
            s_ws = esp_websocket_client_init(&wcfg);
            if (s_ws) {
                esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
                esp_websocket_client_start(s_ws);
                ESP_LOGI(TAG, "Starting py_web WebSocket client -> %s", active_url);
            }
        }
        if (s_connected && s_online) {
            send_heartbeat();
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t ws_gateway_init(void)
{
#if CONFIG_FEATURE_OTA
    ota_mgr_set_progress_cb(on_ws_ota_progress);
#endif
    s_rx_queue = xQueueCreate(64, sizeof(char *));
    xTaskCreate(ws_worker_task, "ws_worker", 6144, NULL, 5, NULL);
    xTaskCreate(ws_mgr_task, "ws_mgr", 4096, NULL, 3, NULL);
    return ESP_OK;
}

bool ws_gateway_is_online(void)
{
    return s_online;
}

#else
esp_err_t ws_gateway_init(void) { return ESP_OK; }
bool ws_gateway_is_online(void) { return false; }
#endif
