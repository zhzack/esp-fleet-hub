#include "mod_usb_otg.h"
#include "sdkconfig.h"

#if CONFIG_FEATURE_USB_OTG && (CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4)

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "app_config.h"
#include "web_server.h"

static const char *TAG = "mod_usb_otg";

#define RID_KBD      1
#define RID_MOUSE    2
#define RID_CONSUMER 3

static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KBD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(RID_MOUSE)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(RID_CONSUMER)),
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

static bool lookup_consumer_name(const char *raw, uint16_t *out_code)
{
    if (!raw || !raw[0] || !out_code) return false;
    const char *n = (strncasecmp(raw, "KEY_", 4) == 0) ? (raw + 4) : raw;
    if (strcasecmp(n, "MUTE") == 0 || strcasecmp(n, "MEDIA_MUTE") == 0) { *out_code = 0x00E2; return true; }
    if (strcasecmp(n, "VOL_UP") == 0 || strcasecmp(n, "VOLUME_UP") == 0) { *out_code = 0x00E9; return true; }
    if (strcasecmp(n, "VOL_DOWN") == 0 || strcasecmp(n, "VOLUME_DOWN") == 0) { *out_code = 0x00EA; return true; }
    if (strcasecmp(n, "PLAY_PAUSE") == 0) { *out_code = 0x00CD; return true; }
    if (strcasecmp(n, "NEXT_TRACK") == 0 || strcasecmp(n, "NEXT") == 0) { *out_code = 0x00B5; return true; }
    if (strcasecmp(n, "PREV_TRACK") == 0 || strcasecmp(n, "PREV") == 0) { *out_code = 0x00B6; return true; }
    if (strcasecmp(n, "STOP") == 0) { *out_code = 0x00B7; return true; }
    if (strcasecmp(n, "POWER") == 0) { *out_code = 0x0030; return true; }
    if (strcasecmp(n, "SLEEP") == 0) { *out_code = 0x0032; return true; }
    if (n[0] == '0' && (n[1] == 'x' || n[1] == 'X')) {
        unsigned long v = strtoul(n + 2, NULL, 16);
        if (v > 0 && v <= 0xFFFF) { *out_code = (uint16_t)v; return true; }
    }
    return false;
}

bool mod_usb_otg_consumer_tap(const char *key_name)
{
    uint16_t code = 0;
    if (!lookup_consumer_name(key_name, &code)) return false;
    if (!s_usb_mounted || !tud_ready()) return false;
    uint32_t w = 0;
    while (!tud_hid_ready() && w++ < 25) vTaskDelay(pdMS_TO_TICKS(2));
    if (!tud_hid_report(RID_CONSUMER, &code, sizeof(code))) return false;
    s_reports_sent++;
    vTaskDelay(pdMS_TO_TICKS(15));
    code = 0;
    tud_hid_report(RID_CONSUMER, &code, sizeof(code));
    s_reports_sent++;
    return true;
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

static esp_err_t read_json_body(httpd_req_t *req, size_t max_body, cJSON **out_root)
{
    int len = req->content_len;
    if (len <= 0 || (size_t)len > max_body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char *buf = calloc(1, (size_t)len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < len) {
        int n = httpd_req_recv(req, buf + received, len - received);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
            return ESP_FAIL;
        }
        received += n;
    }
    buf[received] = '\0';
    *out_root = cJSON_Parse(buf);
    free(buf);
    if (!*out_root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed JSON");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static uint8_t parse_mouse_button_cjson(const cJSON *item, uint8_t def_btn)
{
    if (cJSON_IsNumber(item)) {
        return (uint8_t)item->valueint;
    }
    if (cJSON_IsString(item)) {
        if (strcasecmp(item->valuestring, "right") == 0) return 2;
        if (strcasecmp(item->valuestring, "middle") == 0) return 3;
        return 1;
    }
    return def_btn;
}

static bool exec_action_internal(const cJSON *step, char *err_code, char *err_msg, int depth)
{
    if (!step || !cJSON_IsObject(step)) {
        strcpy(err_code, "E004");
        strcpy(err_msg, "action must be JSON object");
        return false;
    }
    if (depth > 4) {
        strcpy(err_code, "E004");
        strcpy(err_msg, "macro nesting too deep");
        return false;
    }
    const cJSON *jt = cJSON_GetObjectItem(step, "type");
    if (!cJSON_IsString(jt)) {
        strcpy(err_code, "E004");
        strcpy(err_msg, "missing action.type");
        return false;
    }
    const char *t = jt->valuestring;
    if (strcmp(t, "delay") == 0) {
        const cJSON *j_dm = cJSON_GetObjectItem(step, "delay_ms");
        const cJSON *j_ms = cJSON_GetObjectItem(step, "ms");
        int ms = cJSON_IsNumber(j_dm) ? j_dm->valueint : (cJSON_IsNumber(j_ms) ? j_ms->valueint : 0);
        if (ms > 30000) ms = 30000;
        if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
        return true;
    }
    if (strcmp(t, "macro") == 0) {
        const cJSON *steps = cJSON_GetObjectItem(step, "steps");
        if (!cJSON_IsArray(steps)) {
            strcpy(err_code, "E004");
            strcpy(err_msg, "macro requires steps array");
            return false;
        }
        const cJSON *sub = NULL;
        cJSON_ArrayForEach(sub, steps) {
            if (!exec_action_internal(sub, err_code, err_msg, depth + 1)) return false;
        }
        return true;
    }
    if (!mod_usb_otg_is_mounted()) {
        strcpy(err_code, "E007");
        strcpy(err_msg, "USB host not mounted");
        return false;
    }
    if (strcmp(t, "mouse_move") == 0) {
        int dx = cJSON_IsNumber(cJSON_GetObjectItem(step, "x")) ? cJSON_GetObjectItem(step, "x")->valueint :
                 (cJSON_IsNumber(cJSON_GetObjectItem(step, "dx")) ? cJSON_GetObjectItem(step, "dx")->valueint : 0);
        int dy = cJSON_IsNumber(cJSON_GetObjectItem(step, "y")) ? cJSON_GetObjectItem(step, "y")->valueint :
                 (cJSON_IsNumber(cJSON_GetObjectItem(step, "dy")) ? cJSON_GetObjectItem(step, "dy")->valueint : 0);
        if (dx > 127) dx = 127;
        if (dx < -127) dx = -127;
        if (dy > 127) dy = 127;
        if (dy < -127) dy = -127;
        return mod_usb_otg_mouse_move((int8_t)dx, (int8_t)dy);
    }
    if (strcmp(t, "mouse_scroll") == 0) {
        int hx = cJSON_IsNumber(cJSON_GetObjectItem(step, "x")) ? cJSON_GetObjectItem(step, "x")->valueint :
                 (cJSON_IsNumber(cJSON_GetObjectItem(step, "hx")) ? cJSON_GetObjectItem(step, "hx")->valueint : 0);
        int vy = cJSON_IsNumber(cJSON_GetObjectItem(step, "y")) ? cJSON_GetObjectItem(step, "y")->valueint :
                 (cJSON_IsNumber(cJSON_GetObjectItem(step, "vy")) ? cJSON_GetObjectItem(step, "vy")->valueint : 0);
        if (hx > 127) hx = 127;
        if (hx < -127) hx = -127;
        if (vy > 127) vy = 127;
        if (vy < -127) vy = -127;
        return mod_usb_otg_mouse_scroll((int8_t)vy, (int8_t)hx);
    }
    if (strcmp(t, "mouse_click") == 0) {
        return mod_usb_otg_mouse_click(parse_mouse_button_cjson(cJSON_GetObjectItem(step, "button"), 1));
    }
    if (strcmp(t, "mouse_down") == 0) {
        return mod_usb_otg_mouse_btn(parse_mouse_button_cjson(cJSON_GetObjectItem(step, "button"), 1), true);
    }
    if (strcmp(t, "mouse_up") == 0) {
        return mod_usb_otg_mouse_btn(parse_mouse_button_cjson(cJSON_GetObjectItem(step, "button"), 1), false);
    }
    if (strcmp(t, "keyboard_tap") == 0) {
        const cJSON *jk = cJSON_GetObjectItem(step, "key");
        const cJSON *jm = cJSON_GetObjectItem(step, "modifiers");
        return cJSON_IsString(jk) && mod_usb_otg_key_tap(jk->valuestring, cJSON_IsNumber(jm) ? (uint8_t)jm->valueint : 0);
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
    if (strcmp(t, "consumer") == 0 || strcmp(t, "media") == 0) {
        const cJSON *jk = cJSON_GetObjectItem(step, "key");
        return cJSON_IsString(jk) && mod_usb_otg_consumer_tap(jk->valuestring);
    }
    if (strcmp(t, "text") == 0 || strcmp(t, "text_input") == 0) {
        const cJSON *jc = cJSON_GetObjectItem(step, "content");
        if (!cJSON_IsString(jc)) jc = cJSON_GetObjectItem(step, "text");
        int interval = cJSON_IsNumber(cJSON_GetObjectItem(step, "interval_ms")) ? cJSON_GetObjectItem(step, "interval_ms")->valueint : 5;
        if (interval < 0) interval = 0;
        if (interval > 100) interval = 100;
        return cJSON_IsString(jc) && mod_usb_otg_type_text(jc->valuestring, interval);
    }
    strcpy(err_code, "E004");
    snprintf(err_msg, 64, "unsupported action: %.40s", t);
    return false;
}

bool mod_usb_otg_exec_action(const void *cjson_step, char *err_code, char *err_msg)
{
    char ec_tmp[12] = "E500";
    char em_tmp[64] = "exec failed";
    char *ec = err_code ? err_code : ec_tmp;
    char *em = err_msg ? err_msg : em_tmp;
    return exec_action_internal((const cJSON *)cjson_step, ec, em, 0);
}

static esp_err_t hid_status_get_handler(httpd_req_t *req)
{
    if (!web_server_check_read_auth(req)) return ESP_FAIL;
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
    if (read_json_body(req, 2048, &root) != ESP_OK) return ESP_FAIL;
    const cJSON *j_txt = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(j_txt)) j_txt = cJSON_GetObjectItem(root, "content");
    const cJSON *j_int = cJSON_GetObjectItem(root, "interval_ms");
    int interval = cJSON_IsNumber(j_int) ? j_int->valueint : 6;
    if (interval < 0) interval = 0;
    if (interval > 100) interval = 100;
    bool ok = cJSON_IsString(j_txt) && mod_usb_otg_type_text(j_txt->valuestring, interval);
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
    if (read_json_body(req, 1024, &root) != ESP_OK) return ESP_FAIL;

    uint8_t mod = 0;
    const cJSON *j_mod = cJSON_GetObjectItem(root, "mod");
    if (!cJSON_IsNumber(j_mod)) j_mod = cJSON_GetObjectItem(root, "modifiers");
    if (cJSON_IsNumber(j_mod)) mod = (uint8_t)j_mod->valueint;
    const cJSON *j_code = cJSON_GetObjectItem(root, "keycode");
    const cJSON *j_key = cJSON_GetObjectItem(root, "key");
    const cJSON *j_op = cJSON_GetObjectItem(root, "op");
    const char *op = cJSON_IsString(j_op) ? j_op->valuestring : "tap";
    bool sent = false;
    if (cJSON_IsNumber(j_code)) {
        if (strcasecmp(op, "down") == 0) sent = send_kbd_report(mod, (uint8_t)j_code->valueint);
        else if (strcasecmp(op, "up") == 0) sent = send_kbd_report(0, 0);
        else sent = send_kbd_tap(mod, (uint8_t)j_code->valueint);
    } else if (cJSON_IsString(j_key)) {
        if (strcasecmp(op, "consumer") == 0) sent = mod_usb_otg_consumer_tap(j_key->valuestring);
        else if (strcasecmp(op, "down") == 0) sent = mod_usb_otg_key_down(j_key->valuestring, mod);
        else if (strcasecmp(op, "up") == 0) sent = mod_usb_otg_key_up(j_key->valuestring);
        else {
            sent = mod_usb_otg_key_tap(j_key->valuestring, mod);
            if (!sent) sent = mod_usb_otg_consumer_tap(j_key->valuestring);
        }
    }
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
    if (read_json_body(req, 1024, &root) != ESP_OK) return ESP_FAIL;
    int dx = cJSON_IsNumber(cJSON_GetObjectItem(root, "dx")) ? cJSON_GetObjectItem(root, "dx")->valueint : 0;
    int dy = cJSON_IsNumber(cJSON_GetObjectItem(root, "dy")) ? cJSON_GetObjectItem(root, "dy")->valueint : 0;
    int vy = cJSON_IsNumber(cJSON_GetObjectItem(root, "wheel")) ? cJSON_GetObjectItem(root, "wheel")->valueint :
             (cJSON_IsNumber(cJSON_GetObjectItem(root, "vy")) ? cJSON_GetObjectItem(root, "vy")->valueint : 0);
    int hx = cJSON_IsNumber(cJSON_GetObjectItem(root, "hx")) ? cJSON_GetObjectItem(root, "hx")->valueint : 0;
    const cJSON *j_op = cJSON_GetObjectItem(root, "op");
    const cJSON *j_clk = cJSON_GetObjectItem(root, "click");
    const cJSON *j_btn = cJSON_GetObjectItem(root, "button");
    const char *op = cJSON_IsString(j_op) ? j_op->valuestring : "";

    if (dx > 127) dx = 127;
    if (dx < -127) dx = -127;
    if (dy > 127) dy = 127;
    if (dy < -127) dy = -127;
    if (vy > 127) vy = 127;
    if (vy < -127) vy = -127;
    if (hx > 127) hx = 127;
    if (hx < -127) hx = -127;

    bool sent = false;
    if (strcasecmp(op, "down") == 0) {
        sent = mod_usb_otg_mouse_btn(parse_mouse_button_cjson(j_btn, 1), true);
    } else if (strcasecmp(op, "up") == 0) {
        sent = mod_usb_otg_mouse_btn(parse_mouse_button_cjson(j_btn, 1), false);
    } else if (strcasecmp(op, "click") == 0) {
        sent = mod_usb_otg_mouse_click(parse_mouse_button_cjson(j_btn ? j_btn : j_clk, 1));
    } else if (cJSON_IsString(j_clk)) {
        sent = mod_usb_otg_mouse_click(parse_mouse_button_cjson(j_clk, 1));
    } else if (vy != 0 || hx != 0) {
        sent = mod_usb_otg_mouse_scroll((int8_t)vy, (int8_t)hx);
    } else if (dx != 0 || dy != 0) {
        sent = mod_usb_otg_mouse_move((int8_t)dx, (int8_t)dy);
    }
    cJSON_Delete(root);

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"mounted\":%s,\"sent\":%s}",
             s_usb_mounted ? "true" : "false", sent ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t hid_action_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    send_cors(req);

    cJSON *root = NULL;
    if (read_json_body(req, 8192, &root) != ESP_OK) return ESP_FAIL;

    int64_t t0 = esp_timer_get_time();
    char ec[12] = "E500", em[64] = "exec failed";
    bool ok = mod_usb_otg_exec_action(root, ec, em);
    int ms = (int)((esp_timer_get_time() - t0) / 1000LL);
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    cJSON_AddBoolToObject(resp, "mounted", s_usb_mounted);
    cJSON_AddNumberToObject(resp, "exec_time_ms", ms);
    if (!ok) {
        cJSON_AddStringToObject(resp, "error_code", ec);
        cJSON_AddStringToObject(resp, "error_message", em);
        httpd_resp_set_status(req, strcmp(ec, "E007") == 0 ? "409 Conflict" : "400 Bad Request");
    }
    char *txt = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    if (!txt) return httpd_resp_sendstr(req, "{\"ok\":false}");
    esp_err_t err = httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return err;
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
    httpd_uri_t u_act = { .uri = "/api/hid/action", .method = HTTP_POST, .handler = hid_action_post_handler };
    httpd_register_uri_handler(server, &u_st);
    httpd_register_uri_handler(server, &u_txt);
    httpd_register_uri_handler(server, &u_key);
    httpd_register_uri_handler(server, &u_mse);
    httpd_register_uri_handler(server, &u_act);
    ESP_LOGI(TAG, "USB OTG HID routes registered (/api/hid/status, text, key, mouse, action)");
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
bool mod_usb_otg_consumer_tap(const char *k) { (void)k; return false; }
bool mod_usb_otg_exec_action(const void *s, char *ec, char *em) { (void)s;(void)ec;(void)em; return false; }

#endif
