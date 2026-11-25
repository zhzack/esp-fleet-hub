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
#include "hal_st7735.h"
#include "hal_joystick.h"

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
static bool s_need_full_redraw = true;

static void parse_packet(const char *buf) {
    float cpu = s_metrics.cpu;
    float ram = s_metrics.ram;
    char down[16] = {0};
    char up[16] = {0};
    float disk = s_metrics.disk;
    char uptime[16] = {0};
    char time_str[16] = {0};

    // 1. Standard JSON Format via cJSON: {"cpu": 25, "ram": 60, "down": "2MB/s", ...}
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
        // 2. Compact Stream Format Support: C:22,R:60,DL:...,UL:...,D:...,UP:...,T:...
        const char *p = strstr(buf, "C:");
        if (p) cpu = strtof(p + 2, NULL);

        p = strstr(buf, "R:");
        if (p) ram = strtof(p + 2, NULL);

        p = strstr(buf, "DL:");
        if (p) {
            p += 3;
            const char *end = strchr(p, ',');
            int l = end ? (end - p) : strlen(p);
            if (l > 15) l = 15;
            strncpy(down, p, l);
            down[l] = '\0';
        }

        p = strstr(buf, "UL:");
        if (p) {
            p += 3;
            const char *end = strchr(p, ',');
            int l = end ? (end - p) : strlen(p);
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
            int l = end ? (end - p) : strlen(p);
            if (l > 15) l = 15;
            strncpy(uptime, p, l);
            uptime[l] = '\0';
        }

        p = buf;
        while ((p = strstr(p, "T:")) != NULL) {
            if (p == buf || *(p - 1) == ',') {
                p += 2;
                const char *end = strchr(p, ',');
                int l = end ? (end - p) : strlen(p);
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

static void on_joystick_event(joystick_key_t key) {
    const char *badge = NULL;
    switch (key) {
    case JOYSTICK_UP:    badge = "VOL+";     break;
    case JOYSTICK_DOWN:  badge = "VOL-";     break;
    case JOYSTICK_LEFT:  badge = "PREV";     break;
    case JOYSTICK_RIGHT: badge = "NEXT";     break;
    case JOYSTICK_CENTER:badge = "PLAY/PAU"; break;
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

static void monitor_ui_task(void *pvParameters) {
    char str_buf[32];
    ESP_LOGI(TAG, "ST7735 Portrait 80x160 UI task started.");

    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        pc_metrics_t cur;
        portENTER_CRITICAL(&s_metrics_mux);
        cur = s_metrics;
        portEXIT_CRITICAL(&s_metrics_mux);

        if (s_need_full_redraw) {
            st7735_fill_screen(COLOR_BLACK);
            s_need_full_redraw = false;

            st7735_draw_hline(0, 16, 80, COLOR_DARKGREY);
            st7735_draw_hline(0, 92, 80, COLOR_DARKGREY);
            st7735_draw_hline(0, 129, 80, COLOR_DARKGREY);
            st7735_draw_hline(0, 146, 80, COLOR_DARKGREY);
        }

        // Top Header
        if (cur.time_str[0] != '\0' && strcmp(cur.time_str, "00:00:00") != 0) {
            st7735_draw_string(0, 0, cur.time_str, COLOR_WHITE, COLOR_BLACK);
        } else {
            st7735_draw_string(0, 0, "C3-DISP", COLOR_CYAN, COLOR_BLACK);
        }
        bool active = (now_ms - cur.last_update_ms < 5000);
        st7735_draw_string(60, 0, active ? "OK" : "--", active ? COLOR_GREEN : COLOR_RED, COLOR_BLACK);

        // CPU
        uint16_t cpu_col = (cur.cpu > 80.0f) ? COLOR_RED : ((cur.cpu > 50.0f) ? COLOR_ORANGE : COLOR_GREEN);
        st7735_draw_string(2, 19, "CPU:", COLOR_CYAN, COLOR_BLACK);
        snprintf(str_buf, sizeof(str_buf), "%2d%%", (int)cur.cpu);
        st7735_draw_string(40, 19, str_buf, COLOR_WHITE, COLOR_BLACK);
        st7735_draw_progress_bar(2, 35, 76, 5, cur.cpu, cpu_col);

        // RAM
        uint16_t ram_col = (cur.ram > 85.0f) ? COLOR_RED : ((cur.ram > 70.0f) ? COLOR_ORANGE : COLOR_CYAN);
        st7735_draw_string(2, 44, "RAM:", COLOR_MAGENTA, COLOR_BLACK);
        snprintf(str_buf, sizeof(str_buf), "%2d%%", (int)cur.ram);
        st7735_draw_string(40, 44, str_buf, COLOR_WHITE, COLOR_BLACK);
        st7735_draw_progress_bar(2, 60, 76, 5, cur.ram, ram_col);

        // DISK
        st7735_draw_string(2, 69, "DISK:", COLOR_ORANGE, COLOR_BLACK);
        snprintf(str_buf, sizeof(str_buf), "%2d%%", (int)cur.disk);
        st7735_draw_string(44, 69, str_buf, COLOR_WHITE, COLOR_BLACK);
        st7735_draw_progress_bar(2, 85, 76, 5, cur.disk, COLOR_ORANGE);

        // NETWORK
        st7735_draw_string(2, 95, "D:", COLOR_GREEN, COLOR_BLACK);
        snprintf(str_buf, sizeof(str_buf), "%-7s", cur.down);
        st7735_draw_string(20, 95, str_buf, COLOR_WHITE, COLOR_BLACK);

        st7735_draw_string(2, 112, "U:", COLOR_CYAN, COLOR_BLACK);
        snprintf(str_buf, sizeof(str_buf), "%-7s", cur.up);
        st7735_draw_string(20, 112, str_buf, COLOR_WHITE, COLOR_BLACK);

        // UPTIME
        st7735_draw_string(2, 131, "UP:", COLOR_YELLOW, COLOR_BLACK);
        snprintf(str_buf, sizeof(str_buf), "%-6s", cur.uptime);
        st7735_draw_string(28, 131, str_buf, COLOR_WHITE, COLOR_BLACK);

        // FOOTER / MEDIA BADGE
        if (now_ms < s_action_badge_until_ms && s_action_badge[0] != '\0') {
            snprintf(str_buf, sizeof(str_buf), "[%-8s]", s_action_badge);
            st7735_draw_string(2, 148, str_buf, COLOR_YELLOW, COLOR_BLACK);
        } else {
            st7735_draw_string(2, 148, active ? "[TELEMETRY]" : "[WAIT PUSH]", active ? COLOR_GREEN : COLOR_DARKGREY, COLOR_BLACK);
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

// REST API Handlers
static esp_err_t monitor_push_post_handler(httpd_req_t *req) {
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

static esp_err_t joystick_state_get_handler(httpd_req_t *req) {
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

esp_err_t mod_pc_monitor_init(void) {
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

#if CONFIG_FEATURE_JOYSTICK
    ESP_LOGI(TAG, "Initializing 5-way joystick driver (GPIO 4,5,8,9,13)...");
    joystick_init(on_joystick_event);
#endif

    xTaskCreate(monitor_ui_task, "mon_ui", 3072, NULL, 4, NULL);
    return ESP_OK;
}

esp_err_t mod_pc_monitor_register_routes(httpd_handle_t server) {
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

    ESP_LOGI(TAG, "PC Monitor routes registered (/api/monitor/push, /api/joystick/state)");
    return ESP_OK;
}

#else

esp_err_t mod_pc_monitor_init(void) { return ESP_OK; }
esp_err_t mod_pc_monitor_register_routes(httpd_handle_t server) { return ESP_OK; }

#endif // CONFIG_FEATURE_PC_MONITOR
