#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <bearssl/bearssl_hmac.h>
#include <bearssl/bearssl_hash.h>

#ifndef FW_VERSION
#define FW_VERSION "1.1.0-esp8266"
#endif
#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif
#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS ""
#endif
#ifndef DEFAULT_MDNS_HOST
#define DEFAULT_MDNS_HOST ""
#endif
#ifndef DEFAULT_HUB_URL
#define DEFAULT_HUB_URL ""
#endif
#ifndef DEFAULT_DEVICE_SECRET
#define DEFAULT_DEVICE_SECRET ""
#endif
#ifndef FEATURE_WEB_UI
#define FEATURE_WEB_UI 1
#endif
#ifndef FEATURE_REST_API
#define FEATURE_REST_API 1
#endif
#ifndef FEATURE_GPIO_CTRL
#define FEATURE_GPIO_CTRL 1
#endif
#ifndef FEATURE_OTA
#define FEATURE_OTA 1
#endif
#ifndef FEATURE_FLEET_AGENT
#define FEATURE_FLEET_AGENT 1
#endif

// ---------------------------------------------------------------------------
// Persistent EEPROM Config (~512B fits cleanly on 1MB Flash without SPIFFS)
// ---------------------------------------------------------------------------
#define CFG_MAGIC 0x45554633UL
struct PinPersist {
    uint8_t pin;
    uint8_t val;
    uint8_t duty;
    char label[14];
};

struct AppCfg {
    uint32_t magic;
    uint32_t config_ver;
    uint16_t web_port;
    char wifi_ssid[33];
    char wifi_pass[65];
    char device_id[32];
    char mdns_host[32];
    char hub_url[96];
    char device_secret[48];
    char api_token[48];
    uint32_t crc32;
};
static_assert(sizeof(AppCfg) == 368, "AppCfg size mismatch with nvs_gen.py");

static AppCfg g_cfg;
static ESP8266WebServer server(80);
static DNSServer dnsServer;

static bool g_ap_active = false;
static bool g_sta_connected = false;
static unsigned long g_boot_ms = 0;
static unsigned long g_sta_start_ms = 0;
static unsigned long g_last_hb_ms = 0;
static unsigned long g_last_reg_try_ms = 0;
static bool g_hub_registered = false;
static char g_active_hub[128] = "";

// WiFi 15s Switch Rollback State
static bool g_wifi_testing = false;
static unsigned long g_wifi_test_start_ms = 0;
static char g_ssid_bak[33] = "";
static char g_pass_bak[65] = "";

// Safe ESP8266EX GPIO Map (Excludes TX=1, RX=3, Flash SPI=6..11, Boot=0/15)
struct SafePin {
    uint8_t pin;
    const char *label;
    uint8_t role; // 1=LED, 2=RELAY, 0=GENERIC
    bool supports_pwm;
    bool active_low;
    uint8_t val;
    uint8_t duty;
    char custom_label[16];
};

static SafePin g_pins[] = {
    { 2,  "LED_BUILTIN", 1, false, true,  0, 0, "" },
    { 4,  "RELAY_4",     2, true,  false, 0, 0, "" },
    { 5,  "PWM_5",       0, true,  false, 0, 0, "" },
    { 12, "GPIO_12",     0, true,  false, 0, 0, "" },
    { 13, "GPIO_13",     0, false, false, 0, 0, "" },
    { 14, "PWM_14",      0, true,  false, 0, 0, "" },
};
static const size_t NUM_PINS = sizeof(g_pins) / sizeof(g_pins[0]);

// OTA State
static char g_ota_last_sha[65] = "";
static char g_ota_last_err[64] = "";
static bool g_ota_in_progress = false;
static String g_pending_pull_url = "";
static String g_pending_pull_sha = "";
static unsigned long g_reboot_at_ms = 0;

// Serial CLI buffer
static char g_cli_buf[160];
static size_t g_cli_len = 0;

// ---------------------------------------------------------------------------
// Helpers: Default Identity, EEPROM, JSON Mini-Parser, HMAC-SHA256
// ---------------------------------------------------------------------------
static String getMacCleanTail() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char tail[7];
    snprintf(tail, sizeof(tail), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    return String(tail);
}

static void applyCompileTimeDefaults() {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.magic = CFG_MAGIC;
    g_cfg.config_ver = 1;
    g_cfg.web_port = 80;
    String tail = getMacCleanTail();
    snprintf(g_cfg.device_id, sizeof(g_cfg.device_id), "esp8266-%s", tail.c_str());
    if (strlen(DEFAULT_MDNS_HOST) > 0) {
        strlcpy(g_cfg.mdns_host, DEFAULT_MDNS_HOST, sizeof(g_cfg.mdns_host));
    } else {
        strlcpy(g_cfg.mdns_host, g_cfg.device_id, sizeof(g_cfg.mdns_host));
    }
    strlcpy(g_cfg.wifi_ssid, DEFAULT_WIFI_SSID, sizeof(g_cfg.wifi_ssid));
    strlcpy(g_cfg.wifi_pass, DEFAULT_WIFI_PASS, sizeof(g_cfg.wifi_pass));
    strlcpy(g_cfg.hub_url, DEFAULT_HUB_URL, sizeof(g_cfg.hub_url));
    strlcpy(g_cfg.device_secret, DEFAULT_DEVICE_SECRET, sizeof(g_cfg.device_secret));
}

static uint32_t calcCfgCrc32(const AppCfg &c) {
    uint32_t crc = 0xFFFFFFFFUL;
    const uint8_t *p = (const uint8_t *)&c;
    size_t len = sizeof(AppCfg) - sizeof(uint32_t);
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return ~crc;
}

static void saveConfig() {
    g_cfg.crc32 = calcCfgCrc32(g_cfg);
    EEPROM.begin(512);
    EEPROM.put(0, g_cfg);
    EEPROM.commit();
    EEPROM.end();
}

static void loadConfig() {
    EEPROM.begin(512);
    EEPROM.get(0, g_cfg);
    EEPROM.end();
    bool valid = (g_cfg.magic == CFG_MAGIC && g_cfg.web_port != 0 && g_cfg.crc32 == calcCfgCrc32(g_cfg));
    if (!valid) {
        applyCompileTimeDefaults();
        saveConfig();
        Serial.printf("[cfg] Initialized default config: id=%s ssid=%s\n", g_cfg.device_id, g_cfg.wifi_ssid);
    } else {
        if (g_cfg.wifi_ssid[0] == '\0' && strlen(DEFAULT_WIFI_SSID) > 0) {
            strlcpy(g_cfg.wifi_ssid, DEFAULT_WIFI_SSID, sizeof(g_cfg.wifi_ssid));
            strlcpy(g_cfg.wifi_pass, DEFAULT_WIFI_PASS, sizeof(g_cfg.wifi_pass));
        }
        if (g_cfg.hub_url[0] == '\0' && strlen(DEFAULT_HUB_URL) > 0) {
            strlcpy(g_cfg.hub_url, DEFAULT_HUB_URL, sizeof(g_cfg.hub_url));
        }
        Serial.printf("[cfg] Loaded EEPROM config: id=%s ssid=%s ver=%lu\n",
                      g_cfg.device_id, g_cfg.wifi_ssid, (unsigned long)g_cfg.config_ver);
    }
}

static void toHex(const uint8_t *buf, size_t len, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i]     = hx[buf[i] >> 4];
        out[2 * i + 1] = hx[buf[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static void hmacSha256Hex(const char *secret, const char *msg, char outHex[65]) {
    br_hmac_key_context kc;
    br_hmac_context hc;
    uint8_t mac[32];
    br_hmac_key_init(&kc, &br_sha256_vtable, secret, strlen(secret));
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, msg, strlen(msg));
    br_hmac_out(&hc, mac);
    toHex(mac, 32, outHex);
}

static bool jsonExtractStr(const String &json, const char *key, String &out) {
    String pat = "\"" + String(key) + "\"";
    int idx = json.indexOf(pat);
    if (idx < 0) return false;
    int colon = json.indexOf(':', idx + pat.length());
    if (colon < 0) return false;
    int q1 = json.indexOf('"', colon + 1);
    if (q1 < 0) return false;
    int objEnd = json.indexOf('}', colon + 1);
    if (objEnd >= 0 && q1 > objEnd) return false;
    int q2 = q1 + 1;
    while (q2 < (int)json.length()) {
        if (json[q2] == '"' && json[q2 - 1] != '\\') break;
        q2++;
    }
    if (q2 >= (int)json.length()) return false;
    out = json.substring(q1 + 1, q2);
    out.replace("\\/", "/");
    return true;
}

static bool jsonExtractInt(const String &json, const char *key, long &out) {
    String pat = "\"" + String(key) + "\"";
    int idx = json.indexOf(pat);
    if (idx < 0) return false;
    int colon = json.indexOf(':', idx + pat.length());
    if (colon < 0) return false;
    int p = colon + 1;
    while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
    if (p >= (int)json.length()) return false;
    if (!(isDigit(json[p]) || json[p] == '-')) return false;
    out = json.substring(p).toInt();
    return true;
}

// ---------------------------------------------------------------------------
// GPIO Control
// ---------------------------------------------------------------------------
static void gpioInitAll() {
#if FEATURE_GPIO_CTRL
    for (size_t i = 0; i < NUM_PINS; i++) {
        pinMode(g_pins[i].pin, OUTPUT);
        digitalWrite(g_pins[i].pin, g_pins[i].active_low ? HIGH : LOW);
        g_pins[i].val = 0;
        g_pins[i].duty = 0;
    }
#endif
}

static bool setPinDigital(int pin, int val) {
    for (size_t i = 0; i < NUM_PINS; i++) {
        if (g_pins[i].pin == pin) {
            g_pins[i].val = val ? 1 : 0;
            g_pins[i].duty = 0;
            int phys = g_pins[i].active_low ? (val ? LOW : HIGH) : (val ? HIGH : LOW);
            digitalWrite(pin, phys);
            return true;
        }
    }
    return false;
}

static bool setPinPwm(int pin, int duty) {
    for (size_t i = 0; i < NUM_PINS; i++) {
        if (g_pins[i].pin == pin && g_pins[i].supports_pwm) {
            if (duty < 0) duty = 0;
            if (duty > 255) duty = 255;
            g_pins[i].duty = (uint8_t)duty;
            g_pins[i].val = (duty > 0) ? 1 : 0;
            analogWrite(pin, g_pins[i].active_low ? (255 - duty) : duty);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// WiFi Manager + SoftAP Captive Portal + 15s Rollback + mDNS
// ---------------------------------------------------------------------------
static void startSoftApPortal() {
    if (g_ap_active) return;
    WiFi.mode(WIFI_AP_STA);
    String tail = getMacCleanTail();
    tail.toUpperCase();
    String apName = "ESP-Setup-" + tail.substring(tail.length() >= 4 ? tail.length() - 4 : 0);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(apName.c_str());
    dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
    g_ap_active = true;
    Serial.printf("[wifi] SoftAP Captive Portal started: %s @ 192.168.4.1\n", apName.c_str());
}

static void startWifiSta() {
    if (g_cfg.wifi_ssid[0] == '\0') {
        startSoftApPortal();
        return;
    }
    WiFi.mode(g_ap_active ? WIFI_AP_STA : WIFI_STA);
    WiFi.persistent(false);
    WiFi.setPhyMode(WIFI_PHY_MODE_11N);
    WiFi.setOutputPower(20.5f);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setAutoReconnect(true);
    WiFi.hostname(g_cfg.mdns_host);
    WiFi.begin(g_cfg.wifi_ssid, g_cfg.wifi_pass);
    g_sta_start_ms = millis();
    Serial.printf("[wifi] Connecting STA to SSID: [%s] (pw_len=%u, mode=%d)...\n",
                  g_cfg.wifi_ssid, (unsigned)strlen(g_cfg.wifi_pass), (int)WiFi.getMode());
}

static void startMdns() {
    if (strlen(g_cfg.mdns_host) == 0) return;
    MDNS.end();
    if (MDNS.begin(g_cfg.mdns_host)) {
        MDNS.addService("http", "tcp", g_cfg.web_port ? g_cfg.web_port : 80);
        Serial.printf("[mdns] Active: http://%s.local\n", g_cfg.mdns_host);
    }
}

// ---------------------------------------------------------------------------
// Web Auth & CORS & JSON Builders
// ---------------------------------------------------------------------------
static void sendCors() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type, X-API-Key, Authorization, X-Firmware-SHA256");
}

static bool checkAuth() {
    if (g_cfg.api_token[0] == '\0') return true;
    String tok = server.header("X-API-Key");
    if (tok.length() == 0) {
        String auth = server.header("Authorization");
        if (auth.startsWith("Bearer ")) tok = auth.substring(7);
    }
    if (tok == String(g_cfg.api_token)) return true;
    sendCors();
    server.send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}");
    return false;
}

static String buildCapsJson() {
    String ip = g_sta_connected ? WiFi.localIP().toString() : (g_ap_active ? WiFi.softAPIP().toString() : "0.0.0.0");
    String s = "{";
    s += "\"ok\":true";
    s += ",\"device_id\":\"" + String(g_cfg.device_id) + "\"";
    s += ",\"mdns_host\":\"" + String(g_cfg.mdns_host) + "\"";
    s += ",\"chip\":\"ESP8266EX\"";
    s += ",\"board\":\"ESP8266 Generic (1MB)\"";
    s += ",\"board_slug\":\"esp8266_1m\"";
    s += ",\"fw_version\":\"" FW_VERSION "\"";
    s += ",\"flash_mb\":1";
    s += ",\"mac\":\"" + WiFi.macAddress() + "\"";
    s += ",\"ip\":\"" + ip + "\"";
    s += ",\"web_port\":" + String(g_cfg.web_port ? g_cfg.web_port : 80);
    s += ",\"auth_required\":" + String(g_cfg.api_token[0] ? "true" : "false");
    s += ",\"ap_active\":" + String(g_ap_active ? "true" : "false");
    s += ",\"config_ver\":" + String(g_cfg.config_ver);
    s += ",\"hub_url\":\"" + String(g_cfg.hub_url) + "\"";
    s += ",\"wifi\":{\"ssid\":\"" + String(g_cfg.wifi_ssid) + "\",\"rssi\":" + String(g_sta_connected ? WiFi.RSSI() : 0) + ",\"connected\":" + String(g_sta_connected ? "true" : "false") + "}";
    s += ",\"memory\":{\"total_heap\":81920,\"free_heap\":" + String(ESP.getFreeHeap()) + ",\"total_psram\":0,\"free_psram\":0}";
    s += ",\"capabilities\":{";
    s += "\"http_server\":{\"enabled\":true}";
    s += ",\"web_ui\":{\"enabled\":" + String(FEATURE_WEB_UI ? "true" : "false") + "}";
    s += ",\"rest_api\":{\"enabled\":" + String(FEATURE_REST_API ? "true" : "false") + "}";
    s += ",\"gpio\":{\"enabled\":" + String(FEATURE_GPIO_CTRL ? "true" : "false") + "}";
    s += ",\"camera\":{\"enabled\":false},\"storage\":{\"enabled\":false},\"display\":{\"enabled\":false},\"usb_otg\":{\"enabled\":false},\"pc_monitor\":{\"enabled\":false},\"joystick\":{\"enabled\":false}";
    s += ",\"ota\":{\"enabled\":" + String(FEATURE_OTA ? "true" : "false") + ",\"endpoint\":\"/api/ota/upload\"}";
    s += ",\"fleet_agent\":{\"enabled\":" + String(FEATURE_FLEET_AGENT ? "true" : "false") + ",\"registered\":" + String(g_hub_registered ? "true" : "false") + ",\"active_hub\":\"" + String(g_active_hub) + "\"}";
    s += ",\"serial_cli\":{\"enabled\":true}";
    s += "}}";
    return s;
}

static String buildConfigJson() {
    String s = "{";
    s += "\"ok\":true";
    s += ",\"wifi_ssid\":\"" + String(g_cfg.wifi_ssid) + "\"";
    s += ",\"has_wifi_pass\":" + String(g_cfg.wifi_pass[0] ? "true" : "false");
    s += ",\"device_id\":\"" + String(g_cfg.device_id) + "\"";
    s += ",\"mdns_host\":\"" + String(g_cfg.mdns_host) + "\"";
    s += ",\"web_port\":" + String(g_cfg.web_port ? g_cfg.web_port : 80);
    s += ",\"hub_url\":\"" + String(g_cfg.hub_url) + "\"";
    s += ",\"has_device_secret\":" + String(g_cfg.device_secret[0] ? "true" : "false");
    s += ",\"auth_required\":" + String(g_cfg.api_token[0] ? "true" : "false");
    s += ",\"config_ver\":" + String(g_cfg.config_ver);
    s += "}";
    return s;
}

static bool applyConfigJson(const String &body) {
    String v;
    long n = 0;
    bool changed = false;
    bool wifiChanged = false;
    String newSsid = g_cfg.wifi_ssid;
    String newPass = g_cfg.wifi_pass;

    if (jsonExtractStr(body, "wifi_ssid", v) && v.length() > 0 && v != g_cfg.wifi_ssid) {
        newSsid = v; wifiChanged = true; changed = true;
    }
    if (jsonExtractStr(body, "wifi_pass", v)) {
        newPass = v; wifiChanged = true; changed = true;
    }
    if (jsonExtractStr(body, "device_id", v) && v.length() > 0) {
        strlcpy(g_cfg.device_id, v.c_str(), sizeof(g_cfg.device_id)); changed = true;
    }
    if (jsonExtractStr(body, "mdns_host", v) && v.length() > 0) {
        strlcpy(g_cfg.mdns_host, v.c_str(), sizeof(g_cfg.mdns_host));
        startMdns();
        changed = true;
    }
    if (jsonExtractStr(body, "hub_url", v)) {
        strlcpy(g_cfg.hub_url, v.c_str(), sizeof(g_cfg.hub_url));
        g_hub_registered = false;
        changed = true;
    }
    if (jsonExtractStr(body, "api_token", v)) {
        strlcpy(g_cfg.api_token, v.c_str(), sizeof(g_cfg.api_token)); changed = true;
    }
    if (jsonExtractInt(body, "config_ver", n) && n > (long)g_cfg.config_ver) {
        g_cfg.config_ver = (uint32_t)n;
    } else if (changed) {
        g_cfg.config_ver++;
    }

    if (wifiChanged) {
        strlcpy(g_ssid_bak, g_cfg.wifi_ssid, sizeof(g_ssid_bak));
        strlcpy(g_pass_bak, g_cfg.wifi_pass, sizeof(g_pass_bak));
        strlcpy(g_cfg.wifi_ssid, newSsid.c_str(), sizeof(g_cfg.wifi_ssid));
        strlcpy(g_cfg.wifi_pass, newPass.c_str(), sizeof(g_cfg.wifi_pass));
        g_wifi_testing = (g_ssid_bak[0] != '\0');
        g_wifi_test_start_ms = millis();
        startWifiSta();
    }
    if (changed) saveConfig();
    return changed;
}

// ---------------------------------------------------------------------------
// Pull OTA with BearSSL SHA-256 verification (1MB safe self-OTA)
// ---------------------------------------------------------------------------
static bool executePullOta(const String &url, const String &expectedSha) {
#if !FEATURE_OTA
    return false;
#else
    if (!url.startsWith("http://")) {
        strlcpy(g_ota_last_err, "http:// url required", sizeof(g_ota_last_err));
        return false;
    }
    g_ota_in_progress = true;
    g_ota_last_err[0] = '\0';
    Serial.printf("[ota] Pulling firmware from %s...\n", url.c_str());

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(client, url)) {
        strlcpy(g_ota_last_err, "http begin failed", sizeof(g_ota_last_err));
        g_ota_in_progress = false;
        return false;
    }
    int code = http.GET();
    if (code != 200) {
        snprintf(g_ota_last_err, sizeof(g_ota_last_err), "http status %d", code);
        http.end();
        g_ota_in_progress = false;
        return false;
    }

    int totalLen = http.getSize();
    size_t maxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (totalLen > 0 && (size_t)totalLen > maxSpace) {
        snprintf(g_ota_last_err, sizeof(g_ota_last_err), "too large (%d > %u)", totalLen, (unsigned)maxSpace);
        http.end();
        g_ota_in_progress = false;
        return false;
    }
    if (!Update.begin(totalLen > 0 ? (size_t)totalLen : maxSpace, U_FLASH)) {
        snprintf(g_ota_last_err, sizeof(g_ota_last_err), "Update.begin err %u", (unsigned)Update.getError());
        http.end();
        g_ota_in_progress = false;
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    br_sha256_context sha;
    br_sha256_init(&sha);
    uint8_t buf[1024];
    int received = 0;
    bool first = true;
    unsigned long lastDataMs = millis();

    while (http.connected() && (totalLen <= 0 || received < totalLen)) {
        size_t avail = stream->available();
        if (avail > 0) {
            int n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
            if (n > 0) {
                lastDataMs = millis();
                if (first) {
                    first = false;
                    if (buf[0] != 0xE9) {
                        strlcpy(g_ota_last_err, "bad magic byte", sizeof(g_ota_last_err));
                        Update.end(false);
                        http.end();
                        g_ota_in_progress = false;
                        return false;
                    }
                }
                br_sha256_update(&sha, buf, n);
                if (Update.write(buf, n) != (size_t)n) {
                    strlcpy(g_ota_last_err, "flash write fail", sizeof(g_ota_last_err));
                    Update.end(false);
                    http.end();
                    g_ota_in_progress = false;
                    return false;
                }
                received += n;
            }
        } else {
            if (millis() - lastDataMs > 10000) break;
            delay(2);
            yield();
        }
    }
    http.end();

    uint8_t digest[32];
    char hexSha[65];
    br_sha256_out(&sha, digest);
    toHex(digest, 32, hexSha);

    if (received == 0 || (totalLen > 0 && received != totalLen)) {
        strlcpy(g_ota_last_err, "truncated download", sizeof(g_ota_last_err));
        Update.end(false);
        g_ota_in_progress = false;
        return false;
    }
    if (expectedSha.length() == 64 && !expectedSha.equalsIgnoreCase(hexSha)) {
        strlcpy(g_ota_last_err, "SHA-256 mismatch", sizeof(g_ota_last_err));
        Update.end(false);
        g_ota_in_progress = false;
        return false;
    }
    if (!Update.end(true)) {
        snprintf(g_ota_last_err, sizeof(g_ota_last_err), "Update.end err %u", (unsigned)Update.getError());
        g_ota_in_progress = false;
        return false;
    }

    strlcpy(g_ota_last_sha, hexSha, sizeof(g_ota_last_sha));
    g_ota_in_progress = false;
    Serial.printf("[ota] Success (%d B, sha256=%s), rebooting...\n", received, hexSha);
    g_reboot_at_ms = millis() + 1200;
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Compact Web UI Dashboard (PROGMEM keeps heap free!)
// ---------------------------------------------------------------------------
#if FEATURE_WEB_UI
static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP8266 Unified Node</title>
<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;margin:0;padding:16px}.wrap{max-width:720px;margin:auto}.card{background:#1e293b;border:1px solid #334155;border-radius:10px;padding:14px;margin-bottom:12px}h2,h3{margin:0 0 10px;color:#38bdf8}.row{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px}input,button{padding:7px 10px;border-radius:6px;border:1px solid #475569;background:#0f172a;color:#f8fafc}button{background:#0284c7;border-color:#0284c7;cursor:pointer}.pin{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid #334155}small{color:#94a3b8}</style></head>
<body><div class="wrap"><div class="card"><h2>⚡ ESP8266 Unified Node</h2><div id="info">Loading...</div></div>
<div class="card"><h3>🔌 GPIO Control</h3><div id="pins"></div></div>
<div class="card"><h3>📡 Wi-Fi & Hub Config</h3><div class="row"><input id="ssid" placeholder="Wi-Fi SSID"><input id="pass" type="password" placeholder="Password"><input id="hub" placeholder="Hub URL http://..."><button onclick="saveCfg()">Save</button></div><div class="row"><button onclick="scanWifi()">Scan Wi-Fi</button><small id="scan"></small></div></div>
<div class="card"><h3>🚀 OTA Update</h3><div class="row"><input type="file" id="fw"><button onclick="uploadOta()">Flash OTA</button><span id="otaMsg"></span></div></div></div>
<script>
const ap = p => fetch(p).then(r=>r.json());
async function load(){const c=await ap('/api/system/capabilities');document.getElementById('info').innerHTML=`ID: <b>${c.device_id}</b> | IP: ${c.ip} | Heap: ${c.memory.free_heap} B | Hub: ${c.fleet_agent?c.capabilities.fleet_agent.active_hub:''}`;document.getElementById('ssid').value=c.wifi.ssid||'';document.getElementById('hub').value=c.hub_url||'';const g=await ap('/api/gpio/pins');document.getElementById('pins').innerHTML=g.pins.map(p=>`<div class="pin"><span>${p.label} (GPIO${p.pin}) = <b>${p.val}</b></span><button onclick="tog(${p.pin},${p.val?0:1})">${p.val?'Turn OFF':'Turn ON'}</button></div>`).join('');}
async function tog(pin,val){await fetch('/api/gpio/set',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({pin,val})});load();}
async function saveCfg(){await fetch('/api/system/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({wifi_ssid:ssid.value,wifi_pass:pass.value,hub_url:hub.value})});alert('Saved!');}
async function scanWifi(){scan.textContent='Scanning...';const r=await ap('/api/wifi/scan');scan.textContent=(r.networks||[]).slice(0,5).map(x=>`${x.ssid}(${x.rssi})`).join(', ');}
async function uploadOta(){const f=fw.files[0];if(!f)return;otaMsg.textContent='Uploading...';const fd=new FormData();fd.append('firmware',f);const r=await fetch('/api/ota/upload',{method:'POST',body:fd}).then(x=>x.json());otaMsg.textContent=r.message||JSON.stringify(r);}
load();
</script></body></html>)HTML";
#endif

// ---------------------------------------------------------------------------
// HTTP Route Registration
// ---------------------------------------------------------------------------
static void setupHttpServer() {
    server.collectHeaders("X-API-Key", "X-Firmware-SHA256");

#if FEATURE_WEB_UI
    server.on("/", HTTP_GET, []() {
        sendCors();
        server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
    });
#endif

    server.on("/api/system/capabilities", HTTP_GET, []() {
        sendCors();
        server.send(200, "application/json", buildCapsJson());
    });

    server.on("/api/system/status", HTTP_GET, []() {
        sendCors();
        String ip = g_sta_connected ? WiFi.localIP().toString() : "0.0.0.0";
        char buf[320];
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"uptime_sec\":%lu,\"free_heap\":%u,\"free_psram\":0,\"wifi\":{\"connected\":%s,\"rssi\":%d,\"ip\":\"%s\",\"ssid\":\"%s\"}}",
            (unsigned long)((millis() - g_boot_ms) / 1000UL),
            (unsigned)ESP.getFreeHeap(),
            g_sta_connected ? "true" : "false",
            g_sta_connected ? WiFi.RSSI() : 0,
            ip.c_str(),
            g_cfg.wifi_ssid);
        server.send(200, "application/json", buf);
    });

    server.on("/api/system/config", HTTP_GET, []() {
        if (!checkAuth()) return;
        sendCors();
        server.send(200, "application/json", buildConfigJson());
    });

    server.on("/api/system/config", HTTP_POST, []() {
        if (!checkAuth()) return;
        sendCors();
        String body = server.arg("plain");
        applyConfigJson(body);
        server.send(200, "application/json", buildConfigJson());
    });

    server.on("/api/wifi/scan", HTTP_GET, []() {
        sendCors();
        int n = WiFi.scanNetworks();
        String s = "{\"ok\":true,\"networks\":[";
        for (int i = 0; i < n && i < 12; i++) {
            if (i > 0) s += ",";
            String ssid = WiFi.SSID(i);
            ssid.replace("\"", "");
            s += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"auth\":" + String(WiFi.encryptionType(i) != ENC_TYPE_NONE ? 1 : 0) + "}";
        }
        s += "]}";
        server.send(200, "application/json", s);
    });

    server.on("/api/system/reboot", HTTP_POST, []() {
        if (!checkAuth()) return;
        sendCors();
        server.send(200, "application/json", "{\"ok\":true,\"message\":\"Rebooting ESP8266...\"}");
        g_reboot_at_ms = millis() + 800;
    });

    server.on("/api/system/factory_reset", HTTP_POST, []() {
        if (!checkAuth()) return;
        sendCors();
        applyCompileTimeDefaults();
        saveConfig();
        server.send(200, "application/json", "{\"ok\":true,\"message\":\"Factory reset complete\"}");
        g_reboot_at_ms = millis() + 800;
    });

#if FEATURE_GPIO_CTRL
    server.on("/api/gpio/pins", HTTP_GET, []() {
        sendCors();
        String s = "{\"ok\":true,\"board\":\"ESP8266 Generic (1MB)\",\"pins\":[";
        for (size_t i = 0; i < NUM_PINS; i++) {
            if (i > 0) s += ",";
            const char *lbl = g_pins[i].custom_label[0] ? g_pins[i].custom_label : g_pins[i].label;
            s += "{\"pin\":" + String(g_pins[i].pin) + ",\"label\":\"" + String(lbl) + "\",\"role\":" + String(g_pins[i].role) +
                 ",\"output\":true,\"pwm\":" + String(g_pins[i].supports_pwm ? "true" : "false") +
                 ",\"active_low\":" + String(g_pins[i].active_low ? "true" : "false") +
                 ",\"val\":" + String(g_pins[i].val) + ",\"duty\":" + String(g_pins[i].duty) + "}";
        }
        s += "]}";
        server.send(200, "application/json", s);
    });

    server.on("/api/gpio/set", HTTP_POST, []() {
        if (!checkAuth()) return;
        sendCors();
        String body = server.arg("plain");
        long pin = -1, val = 0, duty = -1;
        jsonExtractInt(body, "pin", pin);
        bool hasDuty = jsonExtractInt(body, "duty", duty);
        jsonExtractInt(body, "val", val);
        bool ok = hasDuty ? setPinPwm((int)pin, (int)duty) : setPinDigital((int)pin, (int)val);
        char resp[96];
        snprintf(resp, sizeof(resp), "{\"ok\":%s,\"pin\":%ld,\"val\":%ld}", ok ? "true" : "false", pin, val);
        server.send(ok ? 200 : 400, "application/json", resp);
    });
#endif

#if FEATURE_OTA
    server.on("/api/ota/status", HTTP_GET, []() {
        sendCors();
        char buf[384];
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"running_partition\":\"flash_1m\",\"next_partition\":\"ota_self\",\"rollback_state\":\"n/a\",\"ota_in_progress\":%s,\"last_sha256\":\"%s\",\"last_error\":\"%s\",\"version\":\"" FW_VERSION "\",\"free_sketch_space\":%u}",
            g_ota_in_progress ? "true" : "false", g_ota_last_sha, g_ota_last_err, (unsigned)ESP.getFreeSketchSpace());
        server.send(200, "application/json", buf);
    });

    server.on("/api/ota/upload", HTTP_POST,
        []() {
            sendCors();
            bool ok = !Update.hasError() && g_ota_last_err[0] == '\0';
            server.send(ok ? 200 : 400, "application/json",
                        ok ? "{\"ok\":true,\"message\":\"OTA upload OK! Rebooting...\"}" : "{\"ok\":false,\"error\":\"OTA upload failed\"}");
            if (ok) g_reboot_at_ms = millis() + 1000;
        },
        []() {
            HTTPUpload &up = server.upload();
            if (up.status == UPLOAD_FILE_START) {
                g_ota_last_err[0] = '\0';
                size_t maxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
                if (!Update.begin(maxSpace, U_FLASH)) {
                    strlcpy(g_ota_last_err, "begin_fail", sizeof(g_ota_last_err));
                }
            } else if (up.status == UPLOAD_FILE_WRITE && g_ota_last_err[0] == '\0') {
                if (up.currentSize > 0 && up.totalSize == 0 && up.buf[0] != 0xE9) {
                    strlcpy(g_ota_last_err, "bad_magic", sizeof(g_ota_last_err));
                    Update.end(false);
                    return;
                }
                if (Update.write(up.buf, up.currentSize) != up.currentSize) {
                    strlcpy(g_ota_last_err, "write_fail", sizeof(g_ota_last_err));
                }
            } else if (up.status == UPLOAD_FILE_END && g_ota_last_err[0] == '\0') {
                if (!Update.end(true)) {
                    strlcpy(g_ota_last_err, "end_fail", sizeof(g_ota_last_err));
                }
            }
        }
    );

    server.on("/api/ota/pull", HTTP_POST, []() {
        if (!checkAuth()) return;
        sendCors();
        String body = server.arg("plain");
        String url, sha;
        if (!jsonExtractStr(body, "url", url) || !url.startsWith("http://")) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing valid http url\"}");
            return;
        }
        jsonExtractStr(body, "sha256", sha);
        g_pending_pull_url = url;
        g_pending_pull_sha = sha;
        server.send(200, "application/json", "{\"ok\":true,\"message\":\"ESP8266 Pull OTA scheduled\"}");
    });
#endif

    // Captive Portal Probes + CORS Preflight
    auto capProbe = []() {
        sendCors();
        server.sendHeader("Location", "http://192.168.4.1/", true);
        server.send(302, "text/plain", "");
    };
    server.on("/generate_204", HTTP_GET, capProbe);
    server.on("/hotspot-detect.html", HTTP_GET, capProbe);
    server.on("/connecttest.txt", HTTP_GET, capProbe);

    server.onNotFound([]() {
        sendCors();
        if (server.method() == HTTP_OPTIONS) {
            server.send(204, "text/plain", "");
            return;
        }
        if (g_ap_active && !g_sta_connected) {
            server.sendHeader("Location", "http://192.168.4.1/", true);
            server.send(302, "text/plain", "");
            return;
        }
        server.send(404, "application/json", "{\"ok\":false,\"error\":\"Not found\"}");
    });

    server.begin(g_cfg.web_port ? g_cfg.web_port : 80);
    Serial.printf("[http] Server listening on port %u\n", g_cfg.web_port ? g_cfg.web_port : 80);
}

// ---------------------------------------------------------------------------
// Fleet Hub Client (mDNS _esp-hub._tcp + Challenge HMAC-SHA256 + Heartbeat)
// ---------------------------------------------------------------------------
#if FEATURE_FLEET_AGENT
static bool resolveHubUrl(bool forceMdns = false) {
    if (!forceMdns && g_cfg.hub_url[0] != '\0') {
        strlcpy(g_active_hub, g_cfg.hub_url, sizeof(g_active_hub));
        size_t l = strlen(g_active_hub);
        while (l > 0 && g_active_hub[l - 1] == '/') g_active_hub[--l] = '\0';
        return true;
    }
    int n = MDNS.queryService("esp-hub", "tcp");
    if (n > 0) {
        snprintf(g_active_hub, sizeof(g_active_hub), "http://%s:%u",
                 MDNS.IP(0).toString().c_str(), MDNS.port(0) ? MDNS.port(0) : 8000);
        return true;
    }
    return false;
}

static uint8_t s_hub_fail_cnt = 0;

static bool registerWithHub() {
    bool useMdns = (s_hub_fail_cnt >= 3);
    if (!resolveHubUrl(useMdns) && useMdns) {
        if (!resolveHubUrl(false)) return false;
    }
    if (g_active_hub[0] == '\0') return false;

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(4000);

    String nonce = "";
    char proof[65] = "";
    String chUrl = String(g_active_hub) + "/api/fleet/challenge?device_id=" + String(g_cfg.device_id);
    if (http.begin(client, chUrl)) {
        if (http.GET() == 200) {
            jsonExtractStr(http.getString(), "nonce", nonce);
        }
        http.end();
    }
    if (g_cfg.device_secret[0] != '\0' && nonce.length() > 0) {
        hmacSha256Hex(g_cfg.device_secret, nonce.c_str(), proof);
    }

    String regBody = buildCapsJson();
    if (regBody.endsWith("}")) regBody.remove(regBody.length() - 1);
    if (nonce.length() > 0) regBody += ",\"nonce\":\"" + nonce + "\"";
    if (proof[0] != '\0')   regBody += ",\"proof\":\"" + String(proof) + "\"";
    regBody += ",\"total_psram\":0";
    regBody += ",\"config\":" + buildConfigJson();
    regBody += "}";

    String regUrl = String(g_active_hub) + "/api/fleet/register";
    bool ok = false;
    if (http.begin(client, regUrl)) {
        http.addHeader("Content-Type", "application/json");
        int code = http.POST(regBody);
        if (code == 200) {
            ok = true;
            s_hub_fail_cnt = 0;
            Serial.printf("[fleet] Registered cleanly with Fleet Hub (%s)!\n", g_active_hub);
        } else {
            s_hub_fail_cnt++;
            Serial.printf("[fleet] Register HTTP %d (%s)\n", code, g_active_hub);
        }
        http.end();
    } else {
        s_hub_fail_cnt++;
    }
    return ok;
}

static void sendHeartbeat() {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(4000);

    char body[256];
    snprintf(body, sizeof(body),
        "{\"device_id\":\"%s\",\"rssi\":%d,\"free_heap\":%u,\"uptime_sec\":%lu,\"ip\":\"%s\",\"config_ver\":%lu}",
        g_cfg.device_id, WiFi.RSSI(), (unsigned)ESP.getFreeHeap(),
        (unsigned long)((millis() - g_boot_ms) / 1000UL),
        WiFi.localIP().toString().c_str(), (unsigned long)g_cfg.config_ver);

    String url = String(g_active_hub) + "/api/fleet/heartbeat";
    if (http.begin(client, url)) {
        http.addHeader("Content-Type", "application/json");
        int code = http.POST(body);
        if (code == 200) {
            String resp = http.getString();
            if (resp.indexOf("\"reregister\":true") >= 0) {
                g_hub_registered = false;
            }
            int p = resp.indexOf("\"sync_config\":{");
            if (p >= 0) {
                int start = resp.indexOf('{', p);
                int depth = 0;
                bool inStr = false;
                int endIdx = -1;
                for (int i = start; i < (int)resp.length(); i++) {
                    char ch = resp[i];
                    if (ch == '"' && (i == 0 || resp[i - 1] != '\\')) inStr = !inStr;
                    if (!inStr) {
                        if (ch == '{') depth++;
                        else if (ch == '}' && --depth == 0) { endIdx = i; break; }
                    }
                }
                if (endIdx > start) applyConfigJson(resp.substring(start, endIdx + 1));
            }
        } else if (code < 0) {
            g_hub_registered = false;
        }
        http.end();
    }
}
#endif

// ---------------------------------------------------------------------------
// Interactive Serial CLI
// ---------------------------------------------------------------------------
static void handleCliLine(const String &line) {
    String s = line;
    s.trim();
    if (s.length() == 0) return;

    if (s == "help") {
        Serial.println("Commands: status | scan | wifi <ssid> <pass> | hub <url> | id <dev_id> | gpio <pin> <0|1> | reset | reboot");
    } else if (s == "scan") {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(200);
        int n = WiFi.scanNetworks(false, true);
        Serial.printf("WiFi scan (%d networks, status=%d, mode=%d, flashReal=%u):\n",
                      n, (int)WiFi.status(), (int)WiFi.getMode(), (unsigned)ESP.getFlashChipRealSize());
        for (int i = 0; i < n && i < 15; i++) {
            Serial.printf("  [%d] ssid='%s' rssi=%d ch=%d enc=%d\n",
                          i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i), (int)WiFi.encryptionType(i));
        }
        startWifiSta();
    } else if (s == "status") {
        Serial.println(buildCapsJson());
    } else if (s == "reboot") {
        ESP.restart();
    } else if (s == "reset") {
        applyCompileTimeDefaults();
        saveConfig();
        ESP.restart();
    } else if (s.startsWith("wifi ")) {
        int sp = s.indexOf(' ', 5);
        String ss = (sp > 5) ? s.substring(5, sp) : s.substring(5);
        String pw = (sp > 5) ? s.substring(sp + 1) : "";
        strlcpy(g_cfg.wifi_ssid, ss.c_str(), sizeof(g_cfg.wifi_ssid));
        strlcpy(g_cfg.wifi_pass, pw.c_str(), sizeof(g_cfg.wifi_pass));
        g_cfg.config_ver++;
        saveConfig();
        startWifiSta();
    } else if (s.startsWith("hub ")) {
        strlcpy(g_cfg.hub_url, s.substring(4).c_str(), sizeof(g_cfg.hub_url));
        g_hub_registered = false;
        g_cfg.config_ver++;
        saveConfig();
        Serial.printf("Hub set to %s\n", g_cfg.hub_url);
    } else if (s.startsWith("id ")) {
        strlcpy(g_cfg.device_id, s.substring(3).c_str(), sizeof(g_cfg.device_id));
        g_hub_registered = false;
        g_cfg.config_ver++;
        saveConfig();
    } else if (s.startsWith("gpio ")) {
        int sp = s.indexOf(' ', 5);
        if (sp > 5) {
            int p = s.substring(5, sp).toInt();
            int v = s.substring(sp + 1).toInt();
            bool ok = setPinDigital(p, v);
            Serial.printf("gpio(%d, %d) -> %s\n", p, v, ok ? "OK" : "ERR");
        }
    }
}

void setup() {
    g_boot_ms = millis();
    Serial.begin(115200);
    delay(80);
    Serial.println("\n==================================================");
    Serial.println("  ESP Unified Framework (ESP8266EX 1MB) Booting...");
    Serial.println("==================================================");

    loadConfig();
    gpioInitAll();
    startWifiSta();
    setupHttpServer();
    Serial.println("[cli] Ready. Type 'help' + Enter.");
}

void loop() {
    unsigned long now = millis();

    if (g_reboot_at_ms > 0 && now >= g_reboot_at_ms) {
        delay(50);
        ESP.restart();
    }

    // Deferred Pull OTA execution (outside HTTP handler stack)
    if (g_pending_pull_url.length() > 0) {
        String u = g_pending_pull_url;
        String s = g_pending_pull_sha;
        g_pending_pull_url = "";
        g_pending_pull_sha = "";
        executePullOta(u, s);
    }

    // Track STA state & 15s Rollback / Captive Portal fallback
    bool conn = (WiFi.status() == WL_CONNECTED);
    if (conn && !g_sta_connected) {
        g_sta_connected = true;
        if (g_wifi_testing) {
            g_wifi_testing = false;
            g_ssid_bak[0] = '\0';
            Serial.println("[wifi] New SSID verified OK (rollback cancelled).");
        }
        Serial.printf("[wifi] Connected! IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        startMdns();
    } else if (!conn && g_sta_connected) {
        g_sta_connected = false;
        g_hub_registered = false;
    }

    if (!conn && g_wifi_testing && (now - g_wifi_test_start_ms > 15000)) {
        Serial.println("[wifi] 15s switch timeout! Rolling back to backup SSID...");
        g_wifi_testing = false;
        strlcpy(g_cfg.wifi_ssid, g_ssid_bak, sizeof(g_cfg.wifi_ssid));
        strlcpy(g_cfg.wifi_pass, g_pass_bak, sizeof(g_cfg.wifi_pass));
        saveConfig();
        startWifiSta();
    } else if (!conn && !g_ap_active && g_sta_start_ms > 0 && (now - g_sta_start_ms > 20000)) {
        Serial.printf("[wifi] STA connect timeout (status=%d), starting SoftAP fallback\n", (int)WiFi.status());
        startSoftApPortal();
    }

    if (g_ap_active) dnsServer.processNextRequest();
    if (g_sta_connected) MDNS.update();
    server.handleClient();

#if FEATURE_FLEET_AGENT
    if (g_sta_connected) {
        if (!g_hub_registered) {
            if (now - g_last_reg_try_ms > 5000) {
                g_last_reg_try_ms = now;
                g_hub_registered = registerWithHub();
                g_last_hb_ms = now;
            }
        } else if (now - g_last_hb_ms > 12000) {
            g_last_hb_ms = now;
            sendHeartbeat();
        }
    }
#endif

    // Poll Serial CLI
    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            if (g_cli_len > 0) {
                g_cli_buf[g_cli_len] = '\0';
                handleCliLine(String(g_cli_buf));
                g_cli_len = 0;
            }
        } else if (g_cli_len < sizeof(g_cli_buf) - 1) {
            g_cli_buf[g_cli_len++] = c;
        }
    }
}
