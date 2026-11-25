#include "mod_usb_otg.h"
#include "sdkconfig.h"

#if CONFIG_FEATURE_USB_OTG && (CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4)

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "app_config.h"
#include "web_server.h"

static const char *TAG = "mod_usb_otg";

#define RID_KBD   1
#define RID_MOUSE 2

static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KBD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(RID_MOUSE)),
};

static const tusb_desc_device_t s_dev_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x4011,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static char s_prod_name[32] = "ESP32-S3 Unified HID";
static const char *s_str_desc[] = {
    (char[]){0x09, 0x04},
    "Espressif",
    s_prod_name,
    "UFW0001",
    "ESP Unified HID",
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t s_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0, 100),
    9, TUSB_DESC_INTERFACE, 0, 0, 1, TUSB_CLASS_HID, 0, 0, 4,
    9, HID_DESC_TYPE_HID, 0x11, 0x01, 0x21, 1, HID_DESC_TYPE_REPORT, U16_TO_U8S_LE(sizeof(s_hid_report_desc)),
    7, TUSB_DESC_ENDPOINT, 0x81, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(16), 2,
};

static volatile bool s_usb_mounted = false;
static uint32_t s_reports_sent = 0;
static uint8_t s_held_mods = 0;
static uint8_t s_held_code = 0;
static uint8_t s_mouse_btns = 0;

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) { (void)instance; return s_hid_report_desc; }
uint16_t tud_hid_get_report_cb(uint8_t i, uint8_t r, hid_report_type_t t, uint8_t *b, uint16_t l) { (void)i;(void)r;(void)t;(void)b;(void)l; return 0; }
void tud_hid_set_report_cb(uint8_t i, uint8_t r, hid_report_type_t t, uint8_t const *b, uint16_t l) { (void)i;(void)r;(void)t;(void)b;(void)l; }
void tud_mount_cb(void)  { s_usb_mounted = true;  ESP_LOGI(TAG, "USB OTG host mounted HID device"); }
void tud_umount_cb(void) { s_usb_mounted = false; s_held_mods = 0; s_held_code = 0; s_mouse_btns = 0; ESP_LOGI(TAG, "USB OTG host unmounted"); }

static bool send_kbd_report(uint8_t mod, uint8_t code)
{
    if (!s_usb_mounted || !tud_ready()) return false;
    uint8_t keycode[6] = { code, 0, 0, 0, 0, 0 };
    uint32_t w = 0;
    while (!tud_hid_ready() && w++ < 25) vTaskDelay(pdMS_TO_TICKS(2));
    if (!tud_hid_keyboard_report(RID_KBD, mod, keycode)) return false;
    s_reports_sent++;
    return true;
}

static bool send_kbd_tap(uint8_t mod, uint8_t code)
{
    if (!send_kbd_report(mod, code)) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    send_kbd_report(0, 0);
    vTaskDelay(pdMS_TO_TICKS(6));
    return true;
}

static bool ascii_to_hid(char c, uint8_t *out_mod, uint8_t *out_code)
{
    *out_mod = 0;
    *out_code = 0;
    if (c >= 'a' && c <= 'z') { *out_code = HID_KEY_A + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *out_mod = KEYBOARD_MODIFIER_LEFTSHIFT; *out_code = HID_KEY_A + (c - 'A'); return true; }
    if (c >= '1' && c <= '9') { *out_code = HID_KEY_1 + (c - '1'); return true; }
    if (c == '0') { *out_code = HID_KEY_0; return true; }
    switch (c) {
        case '\n': case '\r': *out_code = HID_KEY_ENTER; return true;
        case '\t': *out_code = HID_KEY_TAB; return true;
        case ' ':  *out_code = HID_KEY_SPACE; return true;
        case '-':  *out_code = HID_KEY_MINUS; return true;
        case '_':  *out_mod = KEYBOARD_MODIFIER_LEFTSHIFT; *out_code = HID_KEY_MINUS; return true;
        case '=':  *out_code = HID_KEY_EQUAL; return true;
        case '+':  *out_mod = KEYBOARD_MODIFIER_LEFTSHIFT; *out_code = HID_KEY_EQUAL; return true;
        case '[':  *out_code = HID_KEY_BRACKET_LEFT; return true;
        case ']':  *out_code = HID_KEY_BRACKET_RIGHT; return true;
        case '\\': *out_code = HID_KEY_BACKSLASH; return true;
        case ';':  *out_code = HID_KEY_SEMICOLON; return true;
        case ':':  *out_mod = KEYBOARD_MODIFIER_LEFTSHIFT; *out_code = HID_KEY_SEMICOLON; return true;
        case '\'': *out_code = HID_KEY_APOSTROPHE; return true;
        case '"':  *out_mod = KEYBOARD_MODIFIER_LEFTSHIFT; *out_code = HID_KEY_APOSTROPHE; return true;
        case ',':  *out_code = HID_KEY_COMMA; return true;
        case '.':  *out_code = HID_KEY_PERIOD; return true;
        case '/':  *out_code = HID_KEY_SLASH; return true;
        case '?':  *out_mod = KEYBOARD_MODIFIER_LEFTSHIFT; *out_code = HID_KEY_SLASH; return true;
        case '!':  *out_mod = KEYBOARD_MODIFIER_LEFTSHIFT; *out_code = HID_KEY_1; return true;
        case '@':  *out_mod = KEYBOARD_MODIFIER_LEFTSHIFT; *out_code = HID_KEY_2; return true;
        default: return false;
    }
}

static bool lookup_key_name(const char *raw, uint8_t *out_code, uint8_t *out_mod)
{
    if (!raw || !raw[0]) return false;
    *out_code = 0;
    *out_mod = 0;

    // Strip optional "KEY_" prefix (ble_hid convention: KEY_A, KEY_ENTER, KEY_CTRL_C...)
    const char *n = (strncasecmp(raw, "KEY_", 4) == 0) ? (raw + 4) : raw;

    // Combo shortcuts
    if (strcasecmp(n, "CTRL_C") == 0) { *out_code = HID_KEY_C; *out_mod = KEYBOARD_MODIFIER_LEFTCTRL; return true; }
    if (strcasecmp(n, "CTRL_V") == 0) { *out_code = HID_KEY_V; *out_mod = KEYBOARD_MODIFIER_LEFTCTRL; return true; }
    if (strcasecmp(n, "CTRL_X") == 0) { *out_code = HID_KEY_X; *out_mod = KEYBOARD_MODIFIER_LEFTCTRL; return true; }
    if (strcasecmp(n, "CTRL_Z") == 0) { *out_code = HID_KEY_Z; *out_mod = KEYBOARD_MODIFIER_LEFTCTRL; return true; }
    if (strcasecmp(n, "CTRL_A") == 0) { *out_code = HID_KEY_A; *out_mod = KEYBOARD_MODIFIER_LEFTCTRL; return true; }
    if (strcasecmp(n, "CTRL_S") == 0) { *out_code = HID_KEY_S; *out_mod = KEYBOARD_MODIFIER_LEFTCTRL; return true; }
    if (strcasecmp(n, "ALT_TAB") == 0) { *out_code = HID_KEY_TAB; *out_mod = KEYBOARD_MODIFIER_LEFTALT; return true; }
    if (strcasecmp(n, "ALT_SHIFT_TAB") == 0) { *out_code = HID_KEY_TAB; *out_mod = KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_LEFTSHIFT; return true; }
    if (strcasecmp(n, "GUI_D") == 0) { *out_code = HID_KEY_D; *out_mod = KEYBOARD_MODIFIER_LEFTGUI; return true; }
    if (strcasecmp(n, "GUI_L") == 0) { *out_code = HID_KEY_L; *out_mod = KEYBOARD_MODIFIER_LEFTGUI; return true; }
    if (strcasecmp(n, "GUI_TAB") == 0) { *out_code = HID_KEY_TAB; *out_mod = KEYBOARD_MODIFIER_LEFTGUI; return true; }
    if (strcasecmp(n, "GUI_CTRL_LEFT") == 0) { *out_code = HID_KEY_ARROW_LEFT; *out_mod = KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_LEFTCTRL; return true; }
    if (strcasecmp(n, "GUI_CTRL_RIGHT") == 0) { *out_code = HID_KEY_ARROW_RIGHT; *out_mod = KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_LEFTCTRL; return true; }

    // Pure modifiers
    if (strcasecmp(n, "LEFTCTRL") == 0 || strcasecmp(n, "RIGHTCTRL") == 0 || strcasecmp(n, "CTRL") == 0) { *out_mod = KEYBOARD_MODIFIER_LEFTCTRL; return true; }
    if (strcasecmp(n, "LEFTSHIFT") == 0 || strcasecmp(n, "RIGHTSHIFT") == 0 || strcasecmp(n, "SHIFT") == 0) { *out_mod = KEYBOARD_MODIFIER_LEFTSHIFT; return true; }
    if (strcasecmp(n, "LEFTALT") == 0 || strcasecmp(n, "RIGHTALT") == 0 || strcasecmp(n, "ALT") == 0) { *out_mod = KEYBOARD_MODIFIER_LEFTALT; return true; }
    if (strcasecmp(n, "LEFTGUI") == 0 || strcasecmp(n, "RIGHTGUI") == 0 || strcasecmp(n, "GUI") == 0) { *out_mod = KEYBOARD_MODIFIER_LEFTGUI; return true; }

    // Named keys
    if (strcasecmp(n, "ENTER") == 0)     { *out_code = HID_KEY_ENTER; return true; }
    if (strcasecmp(n, "ESC") == 0)       { *out_code = HID_KEY_ESCAPE; return true; }
    if (strcasecmp(n, "TAB") == 0)       { *out_code = HID_KEY_TAB; return true; }
    if (strcasecmp(n, "SPACE") == 0)     { *out_code = HID_KEY_SPACE; return true; }
    if (strcasecmp(n, "BACKSPACE") == 0) { *out_code = HID_KEY_BACKSPACE; return true; }
    if (strcasecmp(n, "DELETE") == 0)    { *out_code = HID_KEY_DELETE; return true; }
    if (strcasecmp(n, "INSERT") == 0)    { *out_code = HID_KEY_INSERT; return true; }
    if (strcasecmp(n, "HOME") == 0)      { *out_code = HID_KEY_HOME; return true; }
    if (strcasecmp(n, "END") == 0)       { *out_code = HID_KEY_END; return true; }
    if (strcasecmp(n, "PAGEUP") == 0)    { *out_code = HID_KEY_PAGE_UP; return true; }
    if (strcasecmp(n, "PAGEDOWN") == 0)  { *out_code = HID_KEY_PAGE_DOWN; return true; }
    if (strcasecmp(n, "CAPSLOCK") == 0)  { *out_code = HID_KEY_CAPS_LOCK; return true; }
    if (strcasecmp(n, "UP") == 0)        { *out_code = HID_KEY_ARROW_UP; return true; }
    if (strcasecmp(n, "DOWN") == 0)      { *out_code = HID_KEY_ARROW_DOWN; return true; }
    if (strcasecmp(n, "LEFT") == 0)      { *out_code = HID_KEY_ARROW_LEFT; return true; }
    if (strcasecmp(n, "RIGHT") == 0)     { *out_code = HID_KEY_ARROW_RIGHT; return true; }
    if (strcasecmp(n, "MINUS") == 0)     { *out_code = HID_KEY_MINUS; return true; }
    if (strcasecmp(n, "EQUAL") == 0)     { *out_code = HID_KEY_EQUAL; return true; }
    if (strcasecmp(n, "LBRACKET") == 0)  { *out_code = HID_KEY_BRACKET_LEFT; return true; }
    if (strcasecmp(n, "RBRACKET") == 0)  { *out_code = HID_KEY_BRACKET_RIGHT; return true; }
    if (strcasecmp(n, "BACKSLASH") == 0) { *out_code = HID_KEY_BACKSLASH; return true; }
    if (strcasecmp(n, "SEMICOLON") == 0) { *out_code = HID_KEY_SEMICOLON; return true; }
    if (strcasecmp(n, "QUOTE") == 0)     { *out_code = HID_KEY_APOSTROPHE; return true; }
    if (strcasecmp(n, "BACKQUOTE") == 0) { *out_code = HID_KEY_GRAVE; return true; }
    if (strcasecmp(n, "COMMA") == 0)     { *out_code = HID_KEY_COMMA; return true; }
    if (strcasecmp(n, "DOT") == 0)       { *out_code = HID_KEY_PERIOD; return true; }
    if (strcasecmp(n, "SLASH") == 0)     { *out_code = HID_KEY_SLASH; return true; }

    // Function keys F1..F12
    if ((n[0] == 'F' || n[0] == 'f') && isdigit((unsigned char)n[1])) {
        int fn = atoi(n + 1);
        if (fn >= 1 && fn <= 12) { *out_code = HID_KEY_F1 + (fn - 1); return true; }
    }

    // Single character A-Z, 0-9, symbol
    if (strlen(n) == 1) {
        uint8_t m = 0;
        if (ascii_to_hid((char)tolower((unsigned char)n[0]), &m, out_code)) {
            *out_mod = 0;
            return true;
        }
    }
    return false;
}

bool mod_usb_otg_type_text(const char *text, int interval_ms)
{
    if (!text) return false;
    bool any = false;
    for (const char *p = text; *p; p++) {
        uint8_t mod = 0, code = 0;
        if (ascii_to_hid(*p, &mod, &code)) {
            if (send_kbd_tap(mod, code)) any = true;
            if (interval_ms > 0) vTaskDelay(pdMS_TO_TICKS(interval_ms));
        }
    }
    return any;
}

bool mod_usb_otg_key_tap(const char *key_name, uint8_t modifiers)
{
    uint8_t code = 0, mod = 0;
    if (!lookup_key_name(key_name, &code, &mod)) return false;
    return send_kbd_tap(mod | modifiers, code);
}

bool mod_usb_otg_key_down(const char *key_name, uint8_t modifiers)
{
    uint8_t code = 0, mod = 0;
    if (!lookup_key_name(key_name, &code, &mod)) return false;
    s_held_mods |= (mod | modifiers);
    if (code) s_held_code = code;
    return send_kbd_report(s_held_mods, s_held_code);
}

bool mod_usb_otg_key_up(const char *key_name)
{
    uint8_t code = 0, mod = 0;
    if (!lookup_key_name(key_name, &code, &mod)) return false;
    s_held_mods &= ~mod;
    if (code && s_held_code == code) s_held_code = 0;
    return send_kbd_report(s_held_mods, s_held_code);
}

bool mod_usb_otg_mouse_move(int8_t dx, int8_t dy)
{
    if (!s_usb_mounted || !tud_ready()) return false;
    if (!tud_hid_mouse_report(RID_MOUSE, s_mouse_btns, dx, dy, 0, 0)) return false;
    s_reports_sent++;
    return true;
}

bool mod_usb_otg_mouse_scroll(int8_t vertical, int8_t horizontal)
{
    if (!s_usb_mounted || !tud_ready()) return false;
    if (!tud_hid_mouse_report(RID_MOUSE, s_mouse_btns, 0, 0, vertical, horizontal)) return false;
    s_reports_sent++;
    return true;
}

static uint8_t map_mouse_btn(uint8_t button)
{
    if (button == 2) return MOUSE_BUTTON_RIGHT;
    if (button == 3) return MOUSE_BUTTON_MIDDLE;
    return MOUSE_BUTTON_LEFT;
}

bool mod_usb_otg_mouse_click(uint8_t button)
{
    if (!s_usb_mounted || !tud_ready()) return false;
    uint8_t b = map_mouse_btn(button);
    if (!tud_hid_mouse_report(RID_MOUSE, s_mouse_btns | b, 0, 0, 0, 0)) return false;
    s_reports_sent++;
    vTaskDelay(pdMS_TO_TICKS(15));
    tud_hid_mouse_report(RID_MOUSE, s_mouse_btns, 0, 0, 0, 0);
    s_reports_sent++;
    return true;
}

bool mod_usb_otg_mouse_btn(uint8_t button, bool down)
{
    uint8_t b = map_mouse_btn(button);
    if (down) s_mouse_btns |= b;
    else s_mouse_btns &= ~b;
    if (!s_usb_mounted || !tud_ready()) return false;
    if (!tud_hid_mouse_report(RID_MOUSE, s_mouse_btns, 0, 0, 0, 0)) return false;
    s_reports_sent++;
    return true;
}

static void send_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static esp_err_t read_json_body(httpd_req_t *req, cJSON **out_root)
{
    int len = req->content_len;
    if (len <= 0 || len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char *buf = calloc(1, len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int n = httpd_req_recv(req, buf, len);
    if (n <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
        return ESP_FAIL;
    }
    *out_root = cJSON_Parse(buf);
    free(buf);
    if (!*out_root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed JSON");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t hid_status_get_handler(httpd_req_t *req)
{
    send_cors(req);
    char resp[160];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"supported\":true,\"mounted\":%s,\"reports_sent\":%lu}",
             s_usb_mounted ? "true" : "false", (unsigned long)s_reports_sent);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t hid_text_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    send_cors(req);

    cJSON *root = NULL;
    if (read_json_body(req, &root) != ESP_OK) return ESP_FAIL;
    const cJSON *j_txt = cJSON_GetObjectItem(root, "text");
    bool ok = cJSON_IsString(j_txt) && mod_usb_otg_type_text(j_txt->valuestring, 6);
    cJSON_Delete(root);

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"mounted\":%s,\"sent\":%s}",
             s_usb_mounted ? "true" : "false", ok ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t hid_key_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    send_cors(req);

    cJSON *root = NULL;
    if (read_json_body(req, &root) != ESP_OK) return ESP_FAIL;

    uint8_t mod = 0;
    const cJSON *j_mod = cJSON_GetObjectItem(root, "mod");
    if (cJSON_IsNumber(j_mod)) mod = (uint8_t)j_mod->valueint;
    const cJSON *j_code = cJSON_GetObjectItem(root, "keycode");
    const cJSON *j_key = cJSON_GetObjectItem(root, "key");
    bool sent = false;
    if (cJSON_IsNumber(j_code)) sent = send_kbd_tap(mod, (uint8_t)j_code->valueint);
    else if (cJSON_IsString(j_key)) sent = mod_usb_otg_key_tap(j_key->valuestring, mod);
    cJSON_Delete(root);

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"mounted\":%s,\"sent\":%s}",
             s_usb_mounted ? "true" : "false", sent ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t hid_mouse_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    send_cors(req);

    cJSON *root = NULL;
    if (read_json_body(req, &root) != ESP_OK) return ESP_FAIL;
    int8_t dx = (int8_t)(cJSON_IsNumber(cJSON_GetObjectItem(root, "dx")) ? cJSON_GetObjectItem(root, "dx")->valueint : 0);
    int8_t dy = (int8_t)(cJSON_IsNumber(cJSON_GetObjectItem(root, "dy")) ? cJSON_GetObjectItem(root, "dy")->valueint : 0);
    int8_t wh = (int8_t)(cJSON_IsNumber(cJSON_GetObjectItem(root, "wheel")) ? cJSON_GetObjectItem(root, "wheel")->valueint : 0);
    const cJSON *j_clk = cJSON_GetObjectItem(root, "click");
    bool sent = false;
    if (cJSON_IsString(j_clk)) {
        uint8_t b = 1;
        if (strcasecmp(j_clk->valuestring, "right") == 0) b = 2;
        else if (strcasecmp(j_clk->valuestring, "middle") == 0) b = 3;
        sent = mod_usb_otg_mouse_click(b);
    } else if (wh != 0) {
        sent = mod_usb_otg_mouse_scroll(wh, 0);
    } else if (dx != 0 || dy != 0) {
        sent = mod_usb_otg_mouse_move(dx, dy);
    }
    cJSON_Delete(root);

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"mounted\":%s,\"sent\":%s}",
             s_usb_mounted ? "true" : "false", sent ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

esp_err_t mod_usb_otg_init(void)
{
    const app_config_t *cfg = app_config_ptr();
    if (cfg && cfg->device_id[0]) {
        snprintf(s_prod_name, sizeof(s_prod_name), "%.31s", cfg->device_id);
    }

    const tinyusb_config_t tcfg = {
        .device_descriptor = &s_dev_desc,
        .string_descriptor = s_str_desc,
        .string_descriptor_count = sizeof(s_str_desc) / sizeof(s_str_desc[0]),
        .external_phy = false,
        .configuration_descriptor = s_cfg_desc,
    };
    esp_err_t err = tinyusb_driver_install(&tcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB install failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "USB OTG Composite HID (Keyboard + Mouse) initialized on GPIO 19/20");
    return ESP_OK;
}

esp_err_t mod_usb_otg_register_routes(httpd_handle_t server)
{
    httpd_uri_t u_st  = { .uri = "/api/hid/status", .method = HTTP_GET,  .handler = hid_status_get_handler };
    httpd_uri_t u_txt = { .uri = "/api/hid/text",   .method = HTTP_POST, .handler = hid_text_post_handler };
    httpd_uri_t u_key = { .uri = "/api/hid/key",    .method = HTTP_POST, .handler = hid_key_post_handler };
    httpd_uri_t u_mse = { .uri = "/api/hid/mouse",  .method = HTTP_POST, .handler = hid_mouse_post_handler };
    httpd_register_uri_handler(server, &u_st);
    httpd_register_uri_handler(server, &u_txt);
    httpd_register_uri_handler(server, &u_key);
    httpd_register_uri_handler(server, &u_mse);
    ESP_LOGI(TAG, "USB OTG HID routes registered (/api/hid/status, text, key, mouse)");
    return ESP_OK;
}

bool mod_usb_otg_is_mounted(void) { return s_usb_mounted; }

#else

esp_err_t mod_usb_otg_init(void) { return ESP_OK; }
esp_err_t mod_usb_otg_register_routes(httpd_handle_t server) { (void)server; return ESP_OK; }
bool mod_usb_otg_is_mounted(void) { return false; }
bool mod_usb_otg_type_text(const char *t, int i) { (void)t;(void)i; return false; }
bool mod_usb_otg_key_tap(const char *k, uint8_t m) { (void)k;(void)m; return false; }
bool mod_usb_otg_key_down(const char *k, uint8_t m) { (void)k;(void)m; return false; }
bool mod_usb_otg_key_up(const char *k) { (void)k; return false; }
bool mod_usb_otg_mouse_move(int8_t dx, int8_t dy) { (void)dx;(void)dy; return false; }
bool mod_usb_otg_mouse_scroll(int8_t v, int8_t h) { (void)v;(void)h; return false; }
bool mod_usb_otg_mouse_click(uint8_t b) { (void)b; return false; }
bool mod_usb_otg_mouse_btn(uint8_t b, bool d) { (void)b;(void)d; return false; }

#endif
