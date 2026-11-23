/*
 * Protocol-agnostic command dispatcher used by both transports:
 *   - protocol.c  (cloud / WebSocket envelopes, type != "action")
 *   - lan_mode.c  (HTTP REST + /api/cmd)
 *
 * Handlers receive an input payload (cJSON*, may be NULL) and an empty result
 * object (cJSON*, owned by the caller). They return 0 on success or a non-zero
 * code (loosely HTTP-style: 400 bad request, 404 unknown, 500 internal,
 * 502 upstream/HID failure). On non-zero they write an ASCII message into err.
 */
#include "cmd_router.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "app_config.h"
#include "executor.h"
#include "hid_ble.h"
#include "hid_usb.h"
#include "wifi_mgr.h"
#include "ota_mgr.h"
#include "power_sw.h"

static const char *TAG = "cmdr";
#define MAX_HANDLERS 48

typedef struct {
    const char    *type;
    cmd_handler_fn fn;
} entry_t;

static entry_t s_tbl[MAX_HANDLERS];
static int     s_n = 0;
static char    s_device_id[33] = {0};
static char    s_fw[32] = {0};

void cmd_router_set_device_info(const char *device_id, const char *fw)
{
    if (device_id) strlcpy(s_device_id, device_id, sizeof s_device_id);
    if (fw)        strlcpy(s_fw,        fw,        sizeof s_fw);
}

int cmd_router_register(const char *type, cmd_handler_fn fn)
{
    if (!type || !fn || s_n >= MAX_HANDLERS) return -1;
    for (int i = 0; i < s_n; ++i) {
        if (strcmp(s_tbl[i].type, type) == 0) { s_tbl[i].fn = fn; return 0; }
    }
    s_tbl[s_n].type = type;
    s_tbl[s_n].fn   = fn;
    s_n++;
    return 0;
}

int cmd_router_dispatch(const char *type, const cJSON *payload, cJSON *result,
                        char *err, int errlen)
{
    if (!type) { snprintf(err, errlen, "no type"); return 400; }
    for (int i = 0; i < s_n; ++i) {
        if (strcmp(s_tbl[i].type, type) == 0) {
            err[0] = 0;
            return s_tbl[i].fn(payload, result, err, errlen);
        }
    }
    snprintf(err, errlen, "unknown type: %s", type);
    return 404;
}

/* ---------- helpers ---------- */

static void reboot_task(void *arg)
{
    int ms = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(ms > 0 ? ms : 500));
    esp_restart();
}

static void hex_addr(char *buf, int buflen, const uint8_t a[6])
{
    snprintf(buf, buflen, "%02X:%02X:%02X:%02X:%02X:%02X",
             a[0], a[1], a[2], a[3], a[4], a[5]);
}

static bool parse_addr(const char *s, uint8_t out[6])
{
    if (!s) return false;
    unsigned int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; ++i) out[i] = (uint8_t)v[i];
    return true;
}

/* ---------- handlers ---------- */

static int h_get_status(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)err; (void)errlen;
    cJSON_AddStringToObject(r, "device_id", s_device_id);
    cJSON_AddStringToObject(r, "fw",        s_fw);
    cJSON_AddNumberToObject(r, "uptime_ms", (double)(esp_timer_get_time() / 1000));

    cJSON *w = cJSON_AddObjectToObject(r, "wifi");
    char ssid[33] = {0}, ip[16] = {0};
    wifi_mgr_get_ssid(ssid, sizeof ssid);
    wifi_mgr_get_ip  (ip,   sizeof ip);
    cJSON_AddBoolToObject  (w, "connected", wifi_mgr_connected());
    cJSON_AddStringToObject(w, "ssid",      ssid);
    cJSON_AddStringToObject(w, "ip",        ip);
    cJSON_AddNumberToObject(w, "rssi",      wifi_mgr_rssi());

    cJSON *b = cJSON_AddObjectToObject(r, "ble");
    cJSON_AddBoolToObject  (b, "connected", hid_ble_connected());
    cJSON_AddNumberToObject(b, "peers",     hid_ble_conn_count());

    cJSON *u = cJSON_AddObjectToObject(r, "usb");
    cJSON_AddBoolToObject(u, "mounted", hid_usb_mounted());

    /* Top-level fields for easy dashboard access */
    cJSON_AddBoolToObject  (r, "usb_mounted",   hid_usb_mounted());
    cJSON_AddStringToObject(r, "ip",            ip);

    const char *tr = "ble";
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) tr = "usb";
    cJSON_AddStringToObject(r, "transport", tr);
    const char *tm = "auto";
    switch (executor_get_transport()) {
        case EXECUTOR_TRANSPORT_BLE: tm = "ble"; break;
        case EXECUTOR_TRANSPORT_USB: tm = "usb"; break;
        default: break;
    }
    cJSON_AddStringToObject(r, "transport_mode", tm);
    cJSON_AddNumberToObject(r, "free_heap", (double)esp_get_free_heap_size());
    return 0;
}

static int h_wifi_status(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)err; (void)errlen;
    char ssid[33] = {0}, ip[16] = {0};
    wifi_mgr_get_ssid(ssid, sizeof ssid);
    wifi_mgr_get_ip  (ip,   sizeof ip);
    cJSON_AddBoolToObject  (r, "connected", wifi_mgr_connected());
    cJSON_AddStringToObject(r, "ssid",      ssid);
    cJSON_AddStringToObject(r, "ip",        ip);
    cJSON_AddNumberToObject(r, "rssi",      wifi_mgr_rssi());
    return 0;
}

static int h_wifi_scan(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p;
    wifi_scan_entry_t *arr = malloc(WIFI_SCAN_MAX * sizeof *arr);
    if (!arr) { snprintf(err, errlen, "oom"); return 500; }
    int n = WIFI_SCAN_MAX;
    if (wifi_mgr_scan(arr, &n) != ESP_OK) {
        snprintf(err, errlen, "scan failed");
        free(arr);
        return 500;
    }
    cJSON *nets = cJSON_AddArrayToObject(r, "networks");
    for (int i = 0; i < n; ++i) {
        if (!arr[i].ssid[0]) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", arr[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", arr[i].rssi);
        cJSON_AddNumberToObject(o, "auth", arr[i].auth);
        cJSON_AddItemToArray(nets, o);
    }
    free(arr);
    return 0;
}

static int h_wifi_connect(const cJSON *p, cJSON *r, char *err, int errlen)
{
    const cJSON *jssid = cJSON_GetObjectItem(p, "ssid");
    const cJSON *jpass = cJSON_GetObjectItem(p, "pass");
    if (!cJSON_IsString(jssid) || !jssid->valuestring[0]) {
        snprintf(err, errlen, "ssid missing"); return 400;
    }
    const char *ssid = jssid->valuestring;
    const char *pass = cJSON_IsString(jpass) ? jpass->valuestring : "";

    app_config_t cfg;
    app_config_load(&cfg);
    char prev_ssid[APP_SSID_LEN]; strlcpy(prev_ssid, cfg.wifi_ssid, sizeof prev_ssid);
    char prev_pass[APP_PASS_LEN]; strlcpy(prev_pass, cfg.wifi_pass, sizeof prev_pass);

    ESP_LOGI(TAG, "wifi.connect ssid=%s (prev=%s)", ssid, prev_ssid);
    if (wifi_mgr_sta_connect(ssid, pass, 15000) == ESP_OK) {
        strlcpy(cfg.wifi_ssid, ssid, sizeof cfg.wifi_ssid);
        strlcpy(cfg.wifi_pass, pass, sizeof cfg.wifi_pass);
        app_config_save(&cfg);
        char ip[16] = {0};
        wifi_mgr_get_ip(ip, sizeof ip);
        cJSON_AddBoolToObject  (r, "rolled_back", false);
        cJSON_AddStringToObject(r, "ip",          ip);
        return 0;
    }

    ESP_LOGW(TAG, "wifi.connect failed, rolling back to %s", prev_ssid);
    bool rolled = false;
    if (prev_ssid[0] && wifi_mgr_sta_connect(prev_ssid, prev_pass, 15000) == ESP_OK) {
        rolled = true;
    }
    cJSON_AddBoolToObject(r, "rolled_back", rolled);
    snprintf(err, errlen, "connect failed");
    return 502;
}

static int h_ble_info(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)err; (void)errlen;
    int count = hid_ble_conn_count();
    cJSON_AddBoolToObject  (r, "connected",   count > 0);
    cJSON_AddNumberToObject(r, "count",       count);
    cJSON_AddStringToObject(r, "device_name", s_device_id);
    /* 兼容旧字段：首对端；多连接明细见 peers 数组 */
    hid_ble_peer_t cur;
    if (hid_ble_get_current_peer(&cur)) {
        char a[20];
        hex_addr(a, sizeof a, cur.addr);
        cJSON_AddStringToObject(r, "peer", a);
    }
    hid_ble_conn_t conns[3];
    int n = hid_ble_get_peers(conns, 3);
    cJSON *arr = cJSON_AddArrayToObject(r, "peers");
    for (int i = 0; i < n; ++i) {
        char a[20];
        hex_addr(a, sizeof a, conns[i].addr);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "addr",      a);
        cJSON_AddNumberToObject(o, "addr_type", conns[i].addr_type);
        cJSON_AddNumberToObject(o, "itvl_ms",   conns[i].itvl_125us * 1.25);
        cJSON_AddItemToArray(arr, o);
    }
    return 0;
}

static int h_ble_list_bonds(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)err; (void)errlen;
    hid_ble_peer_t list[8];
    int n = hid_ble_list_bonds(list, 8);
    hid_ble_conn_t conns[3];
    int nc = hid_ble_get_peers(conns, 3);
    cJSON *arr = cJSON_AddArrayToObject(r, "bonds");
    for (int i = 0; i < n; ++i) {
        char a[20];
        hex_addr(a, sizeof a, list[i].addr);
        bool online = false;
        for (int j = 0; j < nc; ++j) {
            if (conns[j].addr_type == list[i].addr_type &&
                memcmp(conns[j].addr, list[i].addr, 6) == 0) {
                online = true;
                break;
            }
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "addr",      a);
        cJSON_AddNumberToObject(o, "addr_type", list[i].addr_type);
        cJSON_AddBoolToObject  (o, "connected", online);
        cJSON_AddItemToArray(arr, o);
    }
    return 0;
}

static int h_ble_unpair(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)r;
    const cJSON *ja = cJSON_GetObjectItem(p, "addr");
    const cJSON *jt = cJSON_GetObjectItem(p, "addr_type");
    if (!cJSON_IsString(ja)) { snprintf(err, errlen, "addr missing"); return 400; }
    uint8_t addr[6];
    if (!parse_addr(ja->valuestring, addr)) { snprintf(err, errlen, "bad addr"); return 400; }
    uint8_t at = cJSON_IsNumber(jt) ? (uint8_t)jt->valueint : 0;
    int rc = hid_ble_unpair(at, addr);
    if (rc != 0) { snprintf(err, errlen, "unpair rc=%d", rc); return 502; }
    return 0;
}

static int h_ble_unpair_all(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)r;
    int rc = hid_ble_unpair_all();
    if (rc != 0) { snprintf(err, errlen, "unpair_all rc=%d", rc); return 502; }
    return 0;
}

static int h_ble_disconnect(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)r;
    int rc = hid_ble_disconnect();
    if (rc != 0) { snprintf(err, errlen, "disconnect rc=%d", rc); return 502; }
    return 0;
}

static int h_ble_switch(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)r;
    const cJSON *ja = cJSON_GetObjectItem(p, "addr");
    const cJSON *jt = cJSON_GetObjectItem(p, "addr_type");
    if (!cJSON_IsString(ja)) { snprintf(err, errlen, "addr missing"); return 400; }
    uint8_t addr[6];
    if (!parse_addr(ja->valuestring, addr)) { snprintf(err, errlen, "bad addr"); return 400; }
    uint8_t at = cJSON_IsNumber(jt) ? (uint8_t)jt->valueint : 0;
    int rc = hid_ble_switch_to(at, addr);
    if (rc != 0) { snprintf(err, errlen, "switch rc=%d", rc); return 502; }
    return 0;
}

static int h_ble_scan(const cJSON *p, cJSON *r, char *err, int errlen)
{
    const cJSON *jd = p ? cJSON_GetObjectItem(p, "duration_ms") : NULL;
    int dur = cJSON_IsNumber(jd) ? jd->valueint : 4000;

    hid_ble_scan_entry_t *arr = malloc(HID_BLE_SCAN_MAX * sizeof *arr);
    if (!arr) { snprintf(err, errlen, "oom"); return 500; }
    int n = hid_ble_scan(dur, arr, HID_BLE_SCAN_MAX);
    if (n < 0) {
        snprintf(err, errlen, "scan rc=%d", n);
        free(arr);
        return 500;
    }

    cJSON *devs = cJSON_AddArrayToObject(r, "devices");
    for (int i = 0; i < n; ++i) {
        cJSON *o = cJSON_CreateObject();
        char a[20];
        hex_addr(a, sizeof a, arr[i].addr);
        cJSON_AddStringToObject(o, "addr",      a);
        cJSON_AddNumberToObject(o, "addr_type", arr[i].addr_type);
        cJSON_AddNumberToObject(o, "rssi",      arr[i].rssi);
        cJSON_AddStringToObject(o, "name",      arr[i].name);
        cJSON_AddItemToArray(devs, o);
    }
    free(arr);
    return 0;
}

static int h_reboot(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)r; (void)err; (void)errlen;
    ESP_LOGW(TAG, "reboot requested");
    xTaskCreate(reboot_task, "reboot", 2048, (void *)(intptr_t)500, 5, NULL);
    return 0;
}

/* ---------- usb / transport ---------- */

static int h_usb_info(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)err; (void)errlen;
    cJSON_AddBoolToObject(r, "mounted", hid_usb_mounted());
    return 0;
}

static int h_transport_get(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)err; (void)errlen;
    const char *m = "auto";
    switch (executor_get_transport()) {
        case EXECUTOR_TRANSPORT_BLE:  m = "ble"; break;
        case EXECUTOR_TRANSPORT_USB:  m = "usb"; break;
        case EXECUTOR_TRANSPORT_AUTO: m = "auto"; break;
    }
    const char *active = executor_active_transport() == EXECUTOR_TRANSPORT_USB ? "usb" : "ble";
    cJSON_AddStringToObject(r, "mode",   m);
    cJSON_AddStringToObject(r, "active", active);
    cJSON_AddBoolToObject  (r, "ble_connected", hid_ble_connected());
    cJSON_AddBoolToObject  (r, "usb_mounted",   hid_usb_mounted());
    return 0;
}

/* ---------- transport persistence ---------- */

/* transport.set 仅改 RAM 的话，重启 / OTA 后会退回编译期默认，导致
 * "USB 用着用着变回 BLE"的失联体验。这里把显式选择落到 NVS("app" 命名空间，
 * 键 transport_mode)，开机时在 cmd_router_init 恢复。 */
static void persist_transport(uint8_t mode)
{
    nvs_handle_t h;
    if (nvs_open("app", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "transport_mode", mode);
    nvs_commit(h);
    nvs_close(h);
}

static void restore_transport(void)
{
    nvs_handle_t h;
    uint8_t mode = EXECUTOR_DEFAULT_TRANSPORT_MODE;
    if (nvs_open("app", NVS_READONLY, &h) != ESP_OK) return;
    esp_err_t err = nvs_get_u8(h, "transport_mode", &mode);
    nvs_close(h);
    if (err != ESP_OK) return; /* 从未显式设置过 — 保持编译期默认 */
    if (mode > (uint8_t)EXECUTOR_TRANSPORT_USB) return;
    executor_set_transport((executor_transport_t)mode);
}

static int h_transport_set(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)r;
    const cJSON *jm = cJSON_GetObjectItem(p, "mode");
    if (!cJSON_IsString(jm)) { snprintf(err, errlen, "mode missing"); return 400; }
    const char *m = jm->valuestring;
    executor_transport_t t;
    if      (!strcmp(m, "ble"))  t = EXECUTOR_TRANSPORT_BLE;
    else if (!strcmp(m, "usb"))  t = EXECUTOR_TRANSPORT_USB;
    else if (!strcmp(m, "auto")) t = EXECUTOR_TRANSPORT_AUTO;
    else { snprintf(err, errlen, "bad mode (ble|usb|auto)"); return 400; }
    executor_set_transport(t);
    persist_transport((uint8_t)t);
    return 0;
}

/* `server.set` — 持久化 WebSocket 服务器地址到 NVS。此前只有串口 CLI 的
 * 私有实现能改 server_host/port，收编进内核后三前端共用同一入口。 */
static int h_server_set(const cJSON *p, cJSON *r, char *err, int errlen)
{
    const cJSON *jh = cJSON_GetObjectItem(p, "host");
    const cJSON *jp = cJSON_GetObjectItem(p, "port");
    if (!cJSON_IsString(jh) || !jh->valuestring[0]) {
        snprintf(err, errlen, "host missing"); return 400;
    }
    uint16_t port = 8000;
    if (cJSON_IsNumber(jp) && jp->valueint > 0 && jp->valueint <= 65535) {
        port = (uint16_t)jp->valueint;
    }
    app_config_t cfg;
    if (app_config_load(&cfg) != ESP_OK) {
        snprintf(err, errlen, "config load failed"); return 500;
    }
    strlcpy(cfg.server_host, jh->valuestring, sizeof cfg.server_host);
    cfg.server_port = port;
    if (app_config_save(&cfg) != ESP_OK) {
        snprintf(err, errlen, "config save failed"); return 500;
    }
    cJSON_AddStringToObject(r, "server", cfg.server_host);
    cJSON_AddNumberToObject(r, "port",   cfg.server_port);
    return 0;
}

/* `auth.secret` — 设置/查询设备注册鉴权预共享密钥（写 NVS，重启生效须重连 WS）。
 * secret 参数缺省时仅查询（脱敏显示：前 4 位 + 长度）。 */
static int h_auth_secret(const cJSON *p, cJSON *r, char *err, int errlen)
{
    const cJSON *js = cJSON_GetObjectItem(p, "secret");
    app_config_t cfg;
    if (app_config_load(&cfg) != ESP_OK) {
        snprintf(err, errlen, "config load failed"); return 500;
    }
    if (cJSON_IsString(js) && js->valuestring[0]) {
        if (strlen(js->valuestring) >= APP_SECRET_LEN) {
            snprintf(err, errlen, "secret too long (max %d)", APP_SECRET_LEN - 1); return 400;
        }
        strlcpy(cfg.device_secret, js->valuestring, sizeof cfg.device_secret);
        if (app_config_save(&cfg) != ESP_OK) {
            snprintf(err, errlen, "config save failed"); return 500;
        }
        ESP_LOGI("cmd_router", "device_secret updated (%zu chars)", strlen(cfg.device_secret));
    }
    char masked[12];
    snprintf(masked, sizeof masked, "%.4s...", cfg.device_secret);
    cJSON_AddStringToObject(r, "secret", masked);
    cJSON_AddNumberToObject(r, "secret_len", (int)strlen(cfg.device_secret));
    return 0;
}

/* `action` — one router type that runs the executor-format payload synchronously.
 * Used by LAN /api/action. Cloud's `action` envelope still goes through the
 * executor queue (handled in protocol.c) and never reaches the router. */
static int h_action(const cJSON *p, cJSON *r, char *err, int errlen)
{
    /* 把当前链路状态先填进 result，server 拿到 ack 即可知道这一发究竟打到哪个 peer。
     * 即便 executor 因 E007 拒绝，也保留 peer 字段方便 server 编排。 */
    bool ble_conn = hid_ble_connected();
    bool usb_mnt  = hid_usb_mounted();
    const char *active = (executor_active_transport() == EXECUTOR_TRANSPORT_USB) ? "usb" : "ble";
    cJSON_AddStringToObject(r, "transport", active);
    cJSON_AddBoolToObject  (r, "ble_connected", ble_conn);
    cJSON_AddBoolToObject  (r, "usb_mounted",   usb_mnt);
    if (ble_conn) {
        hid_ble_peer_t cur;
        if (hid_ble_get_current_peer(&cur)) {
            char a[20];
            hex_addr(a, sizeof a, cur.addr);
            cJSON *peer = cJSON_AddObjectToObject(r, "peer");
            cJSON_AddStringToObject(peer, "addr",      a);
            cJSON_AddNumberToObject(peer, "addr_type", cur.addr_type);
        }
    }

    char ec[8] = {0};
    int rc = executor_run_payload_sync(p, ec, sizeof ec, err, errlen);
    return rc;
}

static int h_ota_start(const cJSON *p, cJSON *r, char *err, int errlen)
{
    if (!p) {
        snprintf(err, errlen, "missing payload");
        return 400;
    }
    const cJSON *ju = cJSON_GetObjectItem(p, "url");
    if (!cJSON_IsString(ju) || !ju->valuestring[0]) {
        snprintf(err, errlen, "missing url");
        return 400;
    }
    const cJSON *jv = cJSON_GetObjectItem(p, "version");
    const cJSON *js = cJSON_GetObjectItem(p, "sha256");
    const char *ver = cJSON_IsString(jv) ? jv->valuestring : NULL;
    const char *sha = cJSON_IsString(js) ? js->valuestring : NULL;

    esp_err_t ret = ota_mgr_start(ju->valuestring, ver, sha);
    if (ret != ESP_OK) {
        snprintf(err, errlen, "ota start failed: 0x%x", ret);
        return 500;
    }
    ota_mgr_build_json(r);
    return 0;
}

static int h_ota_status(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p;
    ota_mgr_build_json(r);
    return 0;
}

/* ---------- Semantic convenience commands & IoT config ---------- */

static int h_cmd_shutdown(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)r;
    static const char SHUTDOWN_MACRO[] = 
    "{\"type\":\"macro\",\"steps\":["
    "{\"type\":\"keyboard_down\",\"key\":\"KEY_LEFTGUI\"},"
    "{\"type\":\"keyboard_tap\",\"key\":\"KEY_X\"},"
    "{\"type\":\"keyboard_up\",\"key\":\"KEY_LEFTGUI\"},"
    "{\"type\":\"delay\",\"delay_ms\":300},"
    "{\"type\":\"keyboard_tap\",\"key\":\"KEY_U\"},"
    "{\"type\":\"delay\",\"delay_ms\":200},"
    "{\"type\":\"keyboard_tap\",\"key\":\"KEY_U\"}"
    "]}";
    cJSON *act = cJSON_Parse(SHUTDOWN_MACRO);
    char ec[8] = {0};
    int ret = executor_run_payload_sync(act, ec, sizeof(ec), err, errlen);
    cJSON_Delete(act);
    return ret;
}

static int h_cmd_lock(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)r;
    cJSON *act = cJSON_Parse("{\"type\":\"keyboard_tap\",\"key\":\"KEY_GUI_L\"}");
    char ec[8] = {0};
    int ret = executor_run_payload_sync(act, ec, sizeof(ec), err, errlen);
    cJSON_Delete(act);
    return ret;
}

static int h_cmd_desktop(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)r;
    cJSON *act = cJSON_Parse("{\"type\":\"keyboard_tap\",\"key\":\"KEY_GUI_D\"}");
    char ec[8] = {0};
    int ret = executor_run_payload_sync(act, ec, sizeof(ec), err, errlen);
    cJSON_Delete(act);
    return ret;
}

static int h_cmd_mute(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)r;
    cJSON *act = cJSON_Parse("{\"type\":\"consumer\",\"key\":\"KEY_MUTE\"}");
    char ec[8] = {0};
    int ret = executor_run_payload_sync(act, ec, sizeof(ec), err, errlen);
    cJSON_Delete(act);
    return ret;
}

static int h_cmd_volup(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)r;
    cJSON *act = cJSON_Parse("{\"type\":\"consumer\",\"key\":\"KEY_VOL_UP\"}");
    char ec[8] = {0};
    int ret = 0;
    for (int i = 0; i < 5; i++) {
        ret = executor_run_payload_sync(act, ec, sizeof(ec), err, errlen);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    cJSON_Delete(act);
    return ret;
}

static int h_cmd_voldown(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)r;
    cJSON *act = cJSON_Parse("{\"type\":\"consumer\",\"key\":\"KEY_VOL_DOWN\"}");
    char ec[8] = {0};
    int ret = 0;
    for (int i = 0; i < 5; i++) {
        ret = executor_run_payload_sync(act, ec, sizeof(ec), err, errlen);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    cJSON_Delete(act);
    return ret;
}

static int h_cmd_playpause(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)r;
    cJSON *act = cJSON_Parse("{\"type\":\"consumer\",\"key\":\"KEY_PLAY_PAUSE\"}");
    char ec[8] = {0};
    int ret = executor_run_payload_sync(act, ec, sizeof(ec), err, errlen);
    cJSON_Delete(act);
    return ret;
}

static int h_power_pulse(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)r;
    int ms = 500;
    if (p) {
        const cJSON *jm = cJSON_GetObjectItem(p, "duration_ms");
        if (cJSON_IsNumber(jm)) ms = jm->valueint;
    }
    esp_err_t ret = power_sw_pulse(ms);
    if (ret != ESP_OK) {
        snprintf(err, errlen, "power switch pulse failed: %d", ret);
        return 500;
    }
    return 0;
}


void cmd_router_init(void)
{
    restore_transport();
    cmd_router_register("get_status",     h_get_status);
    cmd_router_register("wifi.status",    h_wifi_status);
    cmd_router_register("wifi.scan",      h_wifi_scan);
    cmd_router_register("wifi.connect",   h_wifi_connect);
    cmd_router_register("ble.info",       h_ble_info);
    cmd_router_register("ble.list_bonds", h_ble_list_bonds);
    cmd_router_register("ble.unpair",     h_ble_unpair);
    cmd_router_register("ble.unpair_all", h_ble_unpair_all);
    cmd_router_register("ble.disconnect", h_ble_disconnect);
    cmd_router_register("ble.switch",     h_ble_switch);
    cmd_router_register("ble.scan",       h_ble_scan);
    cmd_router_register("usb.info",       h_usb_info);
    cmd_router_register("transport.get",  h_transport_get);
    cmd_router_register("transport.set",  h_transport_set);
    cmd_router_register("server.set",     h_server_set);
    cmd_router_register("auth.secret",    h_auth_secret);
    cmd_router_register("reboot",         h_reboot);
    cmd_router_register("action",         h_action);
    cmd_router_register("ota.start",      h_ota_start);
    cmd_router_register("ota.status",     h_ota_status);

    /* 新增语义化指令与小爱配置 */
    cmd_router_register("cmd.shutdown",   h_cmd_shutdown);
    cmd_router_register("cmd.lock",       h_cmd_lock);
    cmd_router_register("cmd.desktop",    h_cmd_desktop);
    cmd_router_register("cmd.mute",       h_cmd_mute);
    cmd_router_register("cmd.volup",      h_cmd_volup);
    cmd_router_register("cmd.voldown",    h_cmd_voldown);
    cmd_router_register("cmd.playpause",  h_cmd_playpause);
    cmd_router_register("power.pulse",    h_power_pulse);
}
