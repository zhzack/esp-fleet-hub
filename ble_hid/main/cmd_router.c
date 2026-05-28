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

#include "app_config.h"
#include "executor.h"
#include "hid_ble.h"
#include "hid_usb.h"
#include "wifi_mgr.h"

static const char *TAG = "cmdr";
#define MAX_HANDLERS 32

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
    cJSON_AddBoolToObject(b, "connected", hid_ble_connected());

    cJSON *u = cJSON_AddObjectToObject(r, "usb");
    cJSON_AddBoolToObject(u, "mounted", hid_usb_mounted());

    const char *tr = "ble";
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) tr = "usb";
    cJSON_AddStringToObject(r, "transport", tr);
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
    wifi_scan_entry_t arr[WIFI_SCAN_MAX];
    int n = WIFI_SCAN_MAX;
    if (wifi_mgr_scan(arr, &n) != ESP_OK) {
        snprintf(err, errlen, "scan failed");
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
    cJSON_AddBoolToObject  (r, "connected",   hid_ble_connected());
    cJSON_AddStringToObject(r, "device_name", s_device_id);
    hid_ble_peer_t cur;
    if (hid_ble_get_current_peer(&cur)) {
        char a[20];
        hex_addr(a, sizeof a, cur.addr);
        cJSON_AddStringToObject(r, "peer", a);
    }
    return 0;
}

static int h_ble_list_bonds(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)p; (void)err; (void)errlen;
    hid_ble_peer_t list[8];
    int n = hid_ble_list_bonds(list, 8);
    cJSON *arr = cJSON_AddArrayToObject(r, "bonds");
    for (int i = 0; i < n; ++i) {
        char a[20];
        hex_addr(a, sizeof a, list[i].addr);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "addr",      a);
        cJSON_AddNumberToObject(o, "addr_type", list[i].addr_type);
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

    hid_ble_scan_entry_t arr[HID_BLE_SCAN_MAX];
    int n = hid_ble_scan(dur, arr, HID_BLE_SCAN_MAX);
    if (n < 0) { snprintf(err, errlen, "scan rc=%d", n); return 500; }

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

static int h_transport_set(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)r;
    const cJSON *jm = cJSON_GetObjectItem(p, "mode");
    if (!cJSON_IsString(jm)) { snprintf(err, errlen, "mode missing"); return 400; }
    const char *m = jm->valuestring;
    if      (!strcmp(m, "ble"))  executor_set_transport(EXECUTOR_TRANSPORT_BLE);
    else if (!strcmp(m, "usb"))  executor_set_transport(EXECUTOR_TRANSPORT_USB);
    else if (!strcmp(m, "auto")) executor_set_transport(EXECUTOR_TRANSPORT_AUTO);
    else { snprintf(err, errlen, "bad mode (ble|usb|auto)"); return 400; }
    return 0;
}

/* `action` — one router type that runs the executor-format payload synchronously.
 * Used by LAN /api/action. Cloud's `action` envelope still goes through the
 * executor queue (handled in protocol.c) and never reaches the router. */
static int h_action(const cJSON *p, cJSON *r, char *err, int errlen)
{
    (void)r;
    char ec[8] = {0};
    int rc = executor_run_payload_sync(p, ec, sizeof ec, err, errlen);
    return rc;
}

void cmd_router_init(void)
{
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
    cmd_router_register("reboot",         h_reboot);
    cmd_router_register("action",         h_action);
}
