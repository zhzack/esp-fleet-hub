#include "mod_pc_monitor.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if CONFIG_FEATURE_PC_MONITOR

#include "cJSON.h"
#include "app_config.h"
#include "hal_st7735.h"
#include "hal_joystick.h"
#include "system_caps.h"
#include "wifi_mgr.h"
#include "web_server.h"

static const char *TAG = "mod_pc_mon";

typedef struct {
    float cpu;
    float ram;
    char down[16];
    char up[16];
    float disk;
    char uptime[16];
    char time_str[16];
    uint32_t last_update_ms;
} pc_metrics_t;

typedef struct {
    uint16_t header_fg;
    uint16_t accent1;
    uint16_t accent2;
    uint16_t accent3;
    uint16_t divider;
    uint16_t ok_col;
    uint16_t warn_col;
    uint16_t err_col;
} disp_palette_t;

static pc_metrics_t s_metrics = {
    .cpu = 0,
    .ram = 0,
    .down = "0 B/s",
    .up = "0 B/s",
    .disk = 0,
    .uptime = "0h 00m",
    .time_str = "00:00:00",
    .last_update_ms = 0
};

static portMUX_TYPE s_metrics_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_action_badge[16] = "";
static int64_t s_action_badge_until_ms = 0;
static char s_last_key_name[16] = "NONE";
static uint32_t s_joystick_seq = 0;
static volatile uint32_t s_cfg_generation = 1;

static const disp_palette_t *resolve_palette(const char *theme)
{
    static const disp_palette_t p_cyber = {
        COLOR_CYAN, COLOR_CYAN, COLOR_MAGENTA, COLOR_ORANGE, COLOR_DARKGREY, COLOR_GREEN, COLOR_ORANGE, COLOR_RED
    };
    static const disp_palette_t p_emerald = {
        COLOR_GREEN, COLOR_GREEN, COLOR_CYAN, COLOR_YELLOW, COLOR_DARKGREY, COLOR_GREEN, COLOR_YELLOW, COLOR_RED
    };
    static const disp_palette_t p_amber = {
        COLOR_YELLOW, COLOR_ORANGE, COLOR_YELLOW, COLOR_WHITE, COLOR_DARKGREY, COLOR_GREEN, COLOR_ORANGE, COLOR_RED
    };
    static const disp_palette_t p_mono = {
        COLOR_WHITE, COLOR_LIGHTGREY, COLOR_WHITE, COLOR_YELLOW, COLOR_DARKGREY, COLOR_WHITE, COLOR_LIGHTGREY, COLOR_RED
    };
    if (!theme) return &p_cyber;
    if (strcmp(theme, "emerald") == 0) return &p_emerald;
    if (strcmp(theme, "amber") == 0) return &p_amber;
    if (strcmp(theme, "mono") == 0) return &p_mono;
    return &p_cyber;
}

static void parse_packet(const char *buf)
{
    float cpu = s_metrics.cpu;
    float ram = s_metrics.ram;
    char down[16] = {0};
    char up[16] = {0};
    float disk = s_metrics.disk;
    char uptime[16] = {0};
    char time_str[16] = {0};

    cJSON *root = cJSON_Parse(buf);
    if (root && cJSON_IsObject(root)) {
        const cJSON *j_cpu = cJSON_GetObjectItem(root, "cpu");
        const cJSON *j_ram = cJSON_GetObjectItem(root, "ram");
        const cJSON *j_disk = cJSON_GetObjectItem(root, "disk");
        const cJSON *j_dl = cJSON_GetObjectItem(root, "down");
        const cJSON *j_ul = cJSON_GetObjectItem(root, "up");
        const cJSON *j_up = cJSON_GetObjectItem(root, "uptime");
        const cJSON *j_t = cJSON_GetObjectItem(root, "time");

        if (cJSON_IsNumber(j_cpu)) cpu = (float)j_cpu->valuedouble;
        if (cJSON_IsNumber(j_ram)) ram = (float)j_ram->valuedouble;
        if (cJSON_IsNumber(j_disk)) disk = (float)j_disk->valuedouble;
        if (cJSON_IsString(j_dl)) strlcpy(down, j_dl->valuestring, sizeof(down));
        if (cJSON_IsString(j_ul)) strlcpy(up, j_ul->valuestring, sizeof(up));
        if (cJSON_IsString(j_up)) strlcpy(uptime, j_up->valuestring, sizeof(uptime));
        if (cJSON_IsString(j_t)) strlcpy(time_str, j_t->valuestring, sizeof(time_str));
        cJSON_Delete(root);
    } else {
        if (root) cJSON_Delete(root);
        const char *p = strstr(buf, "C:");
        if (p) cpu = strtof(p + 2, NULL);

        p = strstr(buf, "R:");
        if (p) ram = strtof(p + 2, NULL);

        p = strstr(buf, "DL:");
        if (p) {
            p += 3;
            const char *end = strchr(p, ',');
            int l = end ? (int)(end - p) : (int)strlen(p);
            if (l > 15) l = 15;
            strncpy(down, p, l);
            down[l] = '\0';
        }

        p = strstr(buf, "UL:");
        if (p) {
            p += 3;
            const char *end = strchr(p, ',');
            int l = end ? (int)(end - p) : (int)strlen(p);
            if (l > 15) l = 15;
            strncpy(up, p, l);
            up[l] = '\0';
        }

        p = strstr(buf, "D:");
        if (p && (p == buf || *(p - 1) == ',')) disk = strtof(p + 2, NULL);

        p = strstr(buf, "UP:");
        if (p) {
            p += 3;
            const char *end = strchr(p, ',');
            int l = end ? (int)(end - p) : (int)strlen(p);
            if (l > 15) l = 15;
            strncpy(uptime, p, l);
            uptime[l] = '\0';
        }

        p = buf;
        while ((p = strstr(p, "T:")) != NULL) {
            if (p == buf || *(p - 1) == ',') {
                p += 2;
                const char *end = strchr(p, ',');
                int l = end ? (int)(end - p) : (int)strlen(p);
                if (l > 15) l = 15;
                strncpy(time_str, p, l);
                time_str[l] = '\0';
                break;
            }
            p += 2;
        }
    }

    portENTER_CRITICAL(&s_metrics_mux);
    if (!isnan(cpu)) s_metrics.cpu = cpu;
    if (!isnan(ram)) s_metrics.ram = ram;
    if (!isnan(disk)) s_metrics.disk = disk;
    if (down[0]) strlcpy(s_metrics.down, down, sizeof(s_metrics.down));
    if (up[0]) strlcpy(s_metrics.up, up, sizeof(s_metrics.up));
    if (uptime[0]) strlcpy(s_metrics.uptime, uptime, sizeof(s_metrics.uptime));
    if (time_str[0]) strlcpy(s_metrics.time_str, time_str, sizeof(s_metrics.time_str));
    s_metrics.last_update_ms = esp_timer_get_time() / 1000;
    portEXIT_CRITICAL(&s_metrics_mux);
}

static void on_joystick_event(joystick_key_t key)
{
    const char *badge = NULL;
    switch (key) {
    case JOYSTICK_UP:     badge = "VOL+";     break;
    case JOYSTICK_DOWN:   badge = "VOL-";     break;
    case JOYSTICK_LEFT:   badge = "PREV";     break;
    case JOYSTICK_RIGHT:  badge = "NEXT";     break;
    case JOYSTICK_CENTER: badge = "PLAY/PAU"; break;
    default: return;
    }

    if (badge) {
        portENTER_CRITICAL(&s_metrics_mux);
        s_joystick_seq++;
        strlcpy(s_last_key_name, badge, sizeof(s_last_key_name));
        strlcpy(s_action_badge, badge, sizeof(s_action_badge));
        s_action_badge_until_ms = (esp_timer_get_time() / 1000) + 700;
        portEXIT_CRITICAL(&s_metrics_mux);
        ESP_LOGI(TAG, "Joystick Action #%lu Triggered: %s", (unsigned long)s_joystick_seq, badge);
    }
}

static void draw_footer(const disp_palette_t *pal, const app_display_config_t *dcfg,
                        const char *default_label, uint16_t default_col, int64_t now_ms)
{
    char buf[24];
    if (now_ms < s_action_badge_until_ms && s_action_badge[0] != '\0') {
        snprintf(buf, sizeof(buf), "[%-8.8s]", s_action_badge);
        st7735_draw_string_padded(0, 148, buf, 10, COLOR_YELLOW, COLOR_BLACK);
    } else if (dcfg->footer_text[0] != '\0') {
        st7735_draw_string_padded(0, 148, dcfg->footer_text, 10, pal->header_fg, COLOR_BLACK);
    } else {
        st7735_draw_string_padded(0, 148, default_label, 10, default_col, COLOR_BLACK);
    }
}

static void render_monitor_mode(const disp_palette_t *pal, const app_display_config_t *dcfg,
                                const pc_metrics_t *cur, int64_t now_ms, bool full_redraw)
{
    char str_buf[32];
    bool active = (cur->last_update_ms > 0) && ((now_ms - (int64_t)cur->last_update_ms) < 5000);

    if (full_redraw) {
        st7735_fill_screen(COLOR_BLACK);
        st7735_draw_hline(0, 16, 80, pal->divider);
        st7735_draw_hline(0, 146, 80, pal->divider);
    }

    // Header (10 chars total: 7-char title + space + 2-char status)
    if (dcfg->header_text[0] != '\0') {
        snprintf(str_buf, sizeof(str_buf), "%-7.7s", dcfg->header_text);
        st7735_draw_string_padded(0, 0, str_buf, 7, pal->header_fg, COLOR_BLACK);
    } else if (dcfg->show_clock && cur->time_str[0] != '\0' && strcmp(cur->time_str, "00:00:00") != 0) {
        st7735_draw_string_padded(0, 0, cur->time_str, 8, COLOR_WHITE, COLOR_BLACK);
    } else {
        st7735_draw_string_padded(0, 0, "C3-DISP", 7, pal->header_fg, COLOR_BLACK);
    }
    st7735_draw_string(64, 0, active ? "OK" : "--", active ? pal->ok_col : pal->err_col, COLOR_BLACK);

    int y = 19;
    if (dcfg->show_cpu && y + 23 <= 145) {
        uint16_t col = (cur->cpu > 80.0f) ? pal->err_col : ((cur->cpu > 50.0f) ? pal->warn_col : pal->ok_col);
        st7735_draw_string(2, y, "CPU:", pal->accent1, COLOR_BLACK);
        snprintf(str_buf, sizeof(str_buf), "%3d%%", (int)cur->cpu);
        st7735_draw_string_padded(40, y, str_buf, 5, COLOR_WHITE, COLOR_BLACK);
        st7735_draw_progress_bar(2, y + 16, 76, 5, cur->cpu, col);
        y += 24;
    }

    if (dcfg->show_ram && y + 23 <= 145) {
        uint16_t col = (cur->ram > 85.0f) ? pal->err_col : ((cur->ram > 70.0f) ? pal->warn_col : pal->accent1);
        st7735_draw_string(2, y, "RAM:", pal->accent2, COLOR_BLACK);
        snprintf(str_buf, sizeof(str_buf), "%3d%%", (int)cur->ram);
        st7735_draw_string_padded(40, y, str_buf, 5, COLOR_WHITE, COLOR_BLACK);
        st7735_draw_progress_bar(2, y + 16, 76, 5, cur->ram, col);
        y += 24;
    }

    if (dcfg->show_disk && y + 23 <= 145) {
        st7735_draw_string(2, y, "DISK:", pal->accent3, COLOR_BLACK);
        snprintf(str_buf, sizeof(str_buf), "%3d%%", (int)cur->disk);
        st7735_draw_string_padded(42, y, str_buf, 4, COLOR_WHITE, COLOR_BLACK);
        st7735_draw_progress_bar(2, y + 16, 76, 5, cur->disk, pal->accent3);
        y += 24;
    }

    if (dcfg->show_net && y + 34 <= 145) {
        if (full_redraw) st7735_draw_hline(0, y, 80, pal->divider);
        y += 3;
        st7735_draw_string(2, y, "D:", pal->ok_col, COLOR_BLACK);
        st7735_draw_string_padded(20, y, cur->down, 7, COLOR_WHITE, COLOR_BLACK);
        st7735_draw_string(2, y + 17, "U:", pal->accent1, COLOR_BLACK);
        st7735_draw_string_padded(20, y + 17, cur->up, 7, COLOR_WHITE, COLOR_BLACK);
        y += 35;
    }

    if (dcfg->show_uptime && y + 18 <= 145) {
        if (full_redraw) st7735_draw_hline(0, y, 80, pal->divider);
        y += 2;
        st7735_draw_string(2, y, "UP:", COLOR_YELLOW, COLOR_BLACK);
        st7735_draw_string_padded(28, y, cur->uptime, 6, COLOR_WHITE, COLOR_BLACK);
        y += 17;
    }

    // If space remains in monitor mode and line0 has note text, show it nicely
    if (dcfg->lines[0][0] != '\0' && y + 16 <= 145) {
        st7735_draw_string_padded(0, y + 1, dcfg->lines[0], 10, pal->accent1, COLOR_BLACK);
    }

    draw_footer(pal, dcfg, active ? "[LIVE OK]" : "[WAIT PC]", active ? pal->ok_col : COLOR_DARKGREY, now_ms);
}

static void render_text_mode(const disp_palette_t *pal, const app_display_config_t *dcfg,
                             int64_t now_ms, bool full_redraw)
{
    if (full_redraw) {
        st7735_fill_screen(COLOR_BLACK);
        st7735_draw_hline(0, 16, 80, pal->divider);
        st7735_draw_hline(0, 146, 80, pal->divider);
    }

    const char *hdr = dcfg->header_text[0] ? dcfg->header_text : app_config_ptr()->device_id;
    st7735_draw_string_padded(0, 0, hdr, 10, pal->header_fg, COLOR_BLACK);

    for (int i = 0; i < APP_DISPLAY_MAX_LINES; i++) {
        uint16_t fg = (i == 0) ? pal->accent1 : ((i == 5) ? pal->accent2 : COLOR_WHITE);
        st7735_draw_string_padded(0, 20 + i * 20, dcfg->lines[i], 10, fg, COLOR_BLACK);
    }

    draw_footer(pal, dcfg, "CUSTOM TXT", COLOR_DARKGREY, now_ms);
}

static void render_status_mode(const disp_palette_t *pal, const app_display_config_t *dcfg,
                               const pc_metrics_t *cur, int64_t now_ms, bool full_redraw)
{
    char buf[48];
    if (full_redraw) {
        st7735_fill_screen(COLOR_BLACK);
        st7735_draw_hline(0, 16, 80, pal->divider);
        st7735_draw_hline(0, 146, 80, pal->divider);
    }

    const app_config_t *cfg = app_config_ptr();
    const char *hdr = dcfg->header_text[0] ? dcfg->header_text : cfg->device_id;
    st7735_draw_string_padded(0, 0, hdr, 10, pal->header_fg, COLOR_BLACK);

    bool wifi_ok = wifi_mgr_connected();
    char ip[32] = "";
    char ssid[33] = "";
    wifi_mgr_get_ip(ip, sizeof(ip));
    wifi_mgr_get_ssid(ssid, sizeof(ssid));

    // Line 1: WiFi SSID
    snprintf(buf, sizeof(buf), "WF:%.32s", wifi_ok ? ssid : "DOWN");
    st7735_draw_string_padded(0, 20, buf, 10, wifi_ok ? pal->ok_col : pal->err_col, COLOR_BLACK);

    // Line 2-3: Full IP across cleanly wrapped 10-char rows if needed
    snprintf(buf, sizeof(buf), "IP:%.31s", ip[0] ? ip : "0.0.0.0");
    st7735_draw_string_padded(0, 39, buf, 10, COLOR_WHITE, COLOR_BLACK);
    if (strlen(buf) > 10) {
        st7735_draw_string_padded(0, 56, buf + 10, 10, pal->accent1, COLOR_BLACK);
    } else {
        snprintf(buf, sizeof(buf), "RSSI:%ddB", wifi_mgr_rssi());
        st7735_draw_string_padded(0, 56, buf, 10, pal->accent1, COLOR_BLACK);
    }

    // Line 4: Free Heap
    snprintf(buf, sizeof(buf), "MEM:%luK", (unsigned long)(esp_get_free_heap_size() / 1024));
    st7735_draw_string_padded(0, 75, buf, 10, pal->accent2, COLOR_BLACK);

    // Line 5: Uptime or Clock
    if (dcfg->show_clock && cur->time_str[0] != '\0' && strcmp(cur->time_str, "00:00:00") != 0) {
        snprintf(buf, sizeof(buf), "TM:%s", cur->time_str);
    } else {
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        snprintf(buf, sizeof(buf), "UP:%luh%02lum", (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60));
    }
    st7735_draw_string_padded(0, 94, buf, 10, pal->accent3, COLOR_BLACK);

    // Line 6: Custom note or line0
    st7735_draw_string_padded(0, 114, dcfg->lines[0], 10, COLOR_WHITE, COLOR_BLACK);
    st7735_draw_string_padded(0, 130, dcfg->lines[1], 10, COLOR_LIGHTGREY, COLOR_BLACK);

    draw_footer(pal, dcfg, wifi_ok ? "[ONLINE]" : "[OFFLINE]", wifi_ok ? pal->ok_col : pal->warn_col, now_ms);
}

static void monitor_ui_task(void *pvParameters)
{
    uint32_t last_gen = 0;
    ESP_LOGI(TAG, "ST7735 Portrait 80x160 configurable UI task started.");

    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        uint32_t gen = s_cfg_generation;
        bool full_redraw = (gen != last_gen);
        last_gen = gen;

        app_display_config_t dcfg = app_config_ptr()->display;
        const disp_palette_t *pal = resolve_palette(dcfg.theme);

        pc_metrics_t cur;
        portENTER_CRITICAL(&s_metrics_mux);
        cur = s_metrics;
        portEXIT_CRITICAL(&s_metrics_mux);

        if (strcmp(dcfg.mode, "off") == 0) {
            if (full_redraw) {
                st7735_set_backlight(0);
                st7735_fill_screen(COLOR_BLACK);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (full_redraw) {
            st7735_set_backlight(dcfg.backlight);
        }

        if (strcmp(dcfg.mode, "text") == 0) {
            render_text_mode(pal, &dcfg, now_ms, full_redraw);
        } else if (strcmp(dcfg.mode, "status") == 0) {
            render_status_mode(pal, &dcfg, &cur, now_ms, full_redraw);
        } else {
            render_monitor_mode(pal, &dcfg, &cur, now_ms, full_redraw);
        }

        vTaskDelay(pdMS_TO_TICKS(full_redraw ? 150 : 250));
    }
}

void mod_pc_monitor_apply_config(void)
{
    const app_display_config_t *dcfg = &app_config_ptr()->display;
    if (strcmp(dcfg->mode, "off") == 0) {
        st7735_set_backlight(0);
    } else {
        st7735_set_backlight(dcfg->backlight);
    }
    s_cfg_generation++;
}

cJSON *mod_pc_monitor_build_status_json(void)
{
    const app_display_config_t *dcfg = &app_config_ptr()->display;
    int64_t now_ms = esp_timer_get_time() / 1000;

    pc_metrics_t cur;
    char key_copy[16];
    uint32_t seq_copy = 0;
    portENTER_CRITICAL(&s_metrics_mux);
    cur = s_metrics;
    strlcpy(key_copy, s_last_key_name, sizeof(key_copy));
    seq_copy = s_joystick_seq;
    portEXIT_CRITICAL(&s_metrics_mux);

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "ready", true);
    cJSON_AddStringToObject(root, "driver", "ST7735_80x160_PORTRAIT");
    cJSON_AddNumberToObject(root, "width", ST7735_WIDTH);
    cJSON_AddNumberToObject(root, "height", ST7735_HEIGHT);
    cJSON_AddBoolToObject(root, "pc_monitor_enabled", true);
    cJSON_AddBoolToObject(root, "telemetry_active",
                          (cur.last_update_ms > 0) && ((now_ms - (int64_t)cur.last_update_ms) < 5000));
    cJSON_AddStringToObject(root, "last_key", key_copy);
    cJSON_AddNumberToObject(root, "joystick_seq", seq_copy);

    app_config_display_to_cjson(dcfg, root);

    cJSON *metrics = cJSON_AddObjectToObject(root, "metrics");
    cJSON_AddNumberToObject(metrics, "cpu", cur.cpu);
    cJSON_AddNumberToObject(metrics, "ram", cur.ram);
    cJSON_AddNumberToObject(metrics, "disk", cur.disk);
    cJSON_AddStringToObject(metrics, "down", cur.down);
    cJSON_AddStringToObject(metrics, "up", cur.up);
    cJSON_AddStringToObject(metrics, "uptime", cur.uptime);
    cJSON_AddStringToObject(metrics, "time", cur.time_str);
    return root;
}

esp_err_t mod_pc_monitor_update_display_json(const char *json_str, bool switch_text_on_lines)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;
    app_config_t updated;
    app_config_get(&updated);
    bool changed = app_config_apply_display_json(json_str, &updated, switch_text_on_lines);
    if (changed) {
        updated.config_ver++;
        esp_err_t err = app_config_save(&updated);
        if (err != ESP_OK) return err;
    }
    mod_pc_monitor_apply_config();
    return ESP_OK;
}

void mod_pc_monitor_update_metrics(float cpu, float ram, float disk, const char *dl, const char *ul, const char *time_str)
{
    portENTER_CRITICAL(&s_metrics_mux);
    if (!isnan(cpu)) s_metrics.cpu = cpu;
    if (!isnan(ram)) s_metrics.ram = ram;
    if (!isnan(disk)) s_metrics.disk = disk;
    if (dl) strlcpy(s_metrics.down, dl, sizeof(s_metrics.down));
    if (ul) strlcpy(s_metrics.up, ul, sizeof(s_metrics.up));
    if (time_str) strlcpy(s_metrics.time_str, time_str, sizeof(s_metrics.time_str));
    s_metrics.last_update_ms = esp_timer_get_time() / 1000;
    portEXIT_CRITICAL(&s_metrics_mux);
}

void mod_pc_monitor_show_status(const char *title, const char *sub)
{
    app_config_t updated;
    app_config_get(&updated);
    strlcpy(updated.display.mode, "text", sizeof(updated.display.mode));
    if (title) strlcpy(updated.display.lines[0], title, sizeof(updated.display.lines[0]));
    if (sub) strlcpy(updated.display.lines[1], sub, sizeof(updated.display.lines[1]));
    app_config_save(&updated);
    mod_pc_monitor_apply_config();
}

// REST API Handlers
static esp_err_t monitor_push_post_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing payload");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    parse_packet(buf);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Screen metrics updated\"}");
}

static esp_err_t joystick_state_get_handler(httpd_req_t *req)
{
    char key_copy[16];
    uint32_t seq_copy = 0;
    portENTER_CRITICAL(&s_metrics_mux);
    strlcpy(key_copy, s_last_key_name, sizeof(key_copy));
    seq_copy = s_joystick_seq;
    portEXIT_CRITICAL(&s_metrics_mux);

    char resp[160];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"seq\":%lu,\"last_key\":\"%s\"}",
             (unsigned long)seq_copy, key_copy);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, resp);
}

#if !CONFIG_FEATURE_DISPLAY
static esp_err_t pcmon_display_status_get_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    cJSON *root = mod_pc_monitor_build_status_json();
    char *out = root ? cJSON_PrintUnformatted(root) : NULL;
    if (root) cJSON_Delete(root);
    if (!out) return httpd_resp_sendstr(req, "{\"ok\":false}");
    esp_err_t err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return err;
}

static esp_err_t pcmon_display_update_handler(httpd_req_t *req, bool switch_text_on_lines)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    int len = req->content_len;
    if (len <= 0 || len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char *body = calloc(1, len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int r = httpd_req_recv(req, body, len);
    if (r <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
        return ESP_FAIL;
    }
    esp_err_t err = mod_pc_monitor_update_display_json(body, switch_text_on_lines);
    free(body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    return pcmon_display_status_get_handler(req);
}

static esp_err_t pcmon_display_config_post_handler(httpd_req_t *req)
{
    return pcmon_display_update_handler(req, false);
}

static esp_err_t pcmon_display_text_post_handler(httpd_req_t *req)
{
    return pcmon_display_update_handler(req, true);
}
#endif

esp_err_t mod_pc_monitor_init(void)
{
    ESP_LOGI(TAG, "Initializing ST7735 80x160 Portrait Display...");
    esp_err_t err = st7735_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ST7735 LCD!");
        return err;
    }

    st7735_fill_screen(COLOR_BLACK);
    st7735_draw_string(8, 30, "ESP UNIFIED", COLOR_CYAN, COLOR_BLACK);
    st7735_draw_string(8, 55, "C3 MONITOR", COLOR_GREEN, COLOR_BLACK);
    st7735_draw_string(4, 90, "Air101 LCD", COLOR_WHITE, COLOR_BLACK);
    st7735_draw_string(4, 115, "Starting...", COLOR_YELLOW, COLOR_BLACK);

    mod_pc_monitor_apply_config();

#if CONFIG_FEATURE_JOYSTICK
    ESP_LOGI(TAG, "Initializing 5-way joystick driver (GPIO 4,5,8,9,13)...");
    joystick_init(on_joystick_event);
#endif

    xTaskCreate(monitor_ui_task, "mon_ui", 4096, NULL, 4, NULL);
    return ESP_OK;
}

esp_err_t mod_pc_monitor_register_routes(httpd_handle_t server)
{
    httpd_uri_t uri_push = {
        .uri = "/api/monitor/push",
        .method = HTTP_POST,
        .handler = monitor_push_post_handler
    };
    httpd_register_uri_handler(server, &uri_push);

    httpd_uri_t uri_joy = {
        .uri = "/api/joystick/state",
        .method = HTTP_GET,
        .handler = joystick_state_get_handler
    };
    httpd_register_uri_handler(server, &uri_joy);

#if !CONFIG_FEATURE_DISPLAY
    httpd_uri_t uri_dst = {
        .uri = "/api/display/status",
        .method = HTTP_GET,
        .handler = pcmon_display_status_get_handler
    };
    httpd_register_uri_handler(server, &uri_dst);

    httpd_uri_t uri_dcfg = {
        .uri = "/api/display/config",
        .method = HTTP_POST,
        .handler = pcmon_display_config_post_handler
    };
    httpd_register_uri_handler(server, &uri_dcfg);

    httpd_uri_t uri_dtxt = {
        .uri = "/api/display/text",
        .method = HTTP_POST,
        .handler = pcmon_display_text_post_handler
    };
    httpd_register_uri_handler(server, &uri_dtxt);
#endif

    ESP_LOGI(TAG, "PC Monitor routes registered (/api/monitor/push, /api/joystick/state, /api/display/*)");
    return ESP_OK;
}

#else

esp_err_t mod_pc_monitor_init(void) { return ESP_OK; }
esp_err_t mod_pc_monitor_register_routes(httpd_handle_t server) { return ESP_OK; }
void mod_pc_monitor_apply_config(void) {}
cJSON *mod_pc_monitor_build_status_json(void) { return NULL; }
esp_err_t mod_pc_monitor_update_display_json(const char *json_str, bool switch_text_on_lines)
{
    (void)json_str;
    (void)switch_text_on_lines;
    return ESP_ERR_NOT_SUPPORTED;
}
void mod_pc_monitor_update_metrics(float cpu, float ram, float disk, const char *dl, const char *ul, const char *time_str)
{
    (void)cpu; (void)ram; (void)disk; (void)dl; (void)ul; (void)time_str;
}
void mod_pc_monitor_show_status(const char *title, const char *sub)
{
    (void)title; (void)sub;
}

#endif // CONFIG_FEATURE_PC_MONITOR