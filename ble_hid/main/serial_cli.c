#include "serial_cli.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "wifi_mgr.h"
#include "hid_ble.h"
#include "hid_usb.h"
#include "executor.h"
#include "keymap.h"

static const char *TAG = "cli";

#define CLI_BUFFER_SIZE 256

static void print_banner(void)
{
    printf("\r\n");
    printf("==================================================\r\n");
    printf("        ESP32-S3 HID Gateway CLI Console           \r\n");
    printf("   Type 'help' to view available commands.         \r\n");
    printf("==================================================\r\n\n");
}

static void print_help(void)
{
    printf("\r\n--- Available Commands ---\r\n");
    printf("  help                    : Show this help message\r\n");
    printf("  status                  : Show device status, WiFi, BLE, USB info\r\n");
    printf("  wifi <ssid> [password]  : Configure WiFi and save to NVS\r\n");
    printf("  wifi_scan               : Scan nearby WiFi access points\r\n");
    printf("  server <host> [port]    : Configure WebSocket server host/port\r\n");
    printf("  transport <auto|ble|usb>: Set active HID transport mode\r\n");
    printf("  tap <KEY_NAME>          : Tap a keyboard key (e.g. tap KEY_A)\r\n");
    printf("  text <string>           : Type text string via HID\r\n");
    printf("  move <dx> <dy>          : Move mouse by delta (e.g. move 20 -10)\r\n");
    printf("  click [1|2|4]           : Click mouse button (1=left, 2=right, 4=mid)\r\n");
    printf("  reboot                  : Restart the ESP32\r\n\r\n");
}

static void cmd_status(void)
{
    const app_config_t *cfg = app_config_get();
    char ip[24] = "0.0.0.0";
    char ssid[34] = "";
    bool wifi_ok = wifi_mgr_connected();
    if (wifi_ok) {
        wifi_mgr_get_ip(ip, sizeof(ip));
        wifi_mgr_get_ssid(ssid, sizeof(ssid));
    }

    executor_transport_t t = executor_get_transport();
    const char *t_mode = (t == EXECUTOR_TRANSPORT_USB) ? "usb" :
                         (t == EXECUTOR_TRANSPORT_BLE) ? "ble" : "auto";
    const char *t_active = (executor_active_transport() == EXECUTOR_TRANSPORT_USB) ? "usb" : "ble";

    printf("\r\n[Device Status]\r\n");
    printf("  Device ID    : %s\r\n", cfg ? cfg->device_id : "unknown");
    printf("  Server Host  : %s:%u\r\n", cfg ? cfg->server_host : "none", cfg ? cfg->server_port : 0);
    printf("  WiFi Status  : %s (SSID: %s, IP: %s, RSSI: %d dBm)\r\n",
           wifi_ok ? "CONNECTED" : "DISCONNECTED",
           wifi_ok ? ssid : (cfg ? cfg->wifi_ssid : ""),
           wifi_ok ? ip : "-",
           wifi_mgr_rssi());
    printf("  USB Status   : %s\r\n", hid_usb_mounted() ? "MOUNTED" : "UNMOUNTED");
    printf("  BLE Status   : %s\r\n", hid_ble_connected() ? "CONNECTED" : "ADVERTISING");
    printf("  Transport    : mode=%s, active=%s\r\n", t_mode, t_active);
    printf("  Free Heap    : %lu bytes\r\n\r\n", (unsigned long)esp_get_free_heap_size());
}

static void cmd_wifi(char *args)
{
    while (*args == ' ') args++;
    if (!*args) {
        printf("Usage: wifi <ssid> [password]\r\n");
        return;
    }

    char ssid[34] = {0};
    char pass[66] = {0};

    char *sep = strchr(args, ' ');
    if (sep) {
        *sep = '\0';
        strlcpy(ssid, args, sizeof(ssid));
        args = sep + 1;
        while (*args == ' ') args++;
        strlcpy(pass, args, sizeof(pass));
    } else {
        strlcpy(ssid, args, sizeof(ssid));
    }

    app_config_t cfg;
    if (app_config_load(&cfg) == ESP_OK) {
        strlcpy(cfg.wifi_ssid, ssid, sizeof(cfg.wifi_ssid));
        strlcpy(cfg.wifi_pass, pass, sizeof(cfg.wifi_pass));
        app_config_save(&cfg);
        printf("Saved WiFi config: SSID='%s'. Reconnecting...\r\n", ssid);
        wifi_mgr_sta_stop();
        esp_err_t err = wifi_mgr_sta_connect(ssid, pass, 15000);
        if (err == ESP_OK) {
            char ip[24] = {0};
            wifi_mgr_get_ip(ip, sizeof(ip));
            printf("WiFi Connected successfully! IP=%s\r\n", ip);
        } else {
            printf("WiFi Connect timed out. Will continue retrying in background.\r\n");
        }
    } else {
        printf("Failed to load app config from NVS.\r\n");
    }
}

static void cmd_wifi_scan(void)
{
    printf("Scanning WiFi networks (takes ~3s)...\r\n");
    wifi_scan_entry_t aps[WIFI_SCAN_MAX];
    int n = 0;
    if (wifi_mgr_scan(aps, &n) == ESP_OK) {
        printf("Found %d access points:\r\n", n);
        for (int i = 0; i < n; ++i) {
            printf("  [%02d] SSID: %-24s  RSSI: %d dBm\r\n", i + 1, aps[i].ssid, aps[i].rssi);
        }
    } else {
        printf("WiFi scan failed.\r\n");
    }
}

static void cmd_server(char *args)
{
    while (*args == ' ') args++;
    if (!*args) {
        printf("Usage: server <host> [port]\r\n");
        return;
    }

    char host[64] = {0};
    uint16_t port = 8000;

    char *sep = strchr(args, ' ');
    if (sep) {
        *sep = '\0';
        strlcpy(host, args, sizeof(host));
        port = (uint16_t)atoi(sep + 1);
        if (port == 0) port = 8000;
    } else {
        strlcpy(host, args, sizeof(host));
    }

    app_config_t cfg;
    if (app_config_load(&cfg) == ESP_OK) {
        strlcpy(cfg.server_host, host, sizeof(cfg.server_host));
        cfg.server_port = port;
        app_config_save(&cfg);
        printf("Saved server config: ws://%s:%u/ws/device\r\n", host, port);
    }
}

static void cmd_transport(char *args)
{
    while (*args == ' ') args++;
    if (!strcmp(args, "auto")) {
        executor_set_transport(EXECUTOR_TRANSPORT_AUTO);
        printf("Transport set to: auto\r\n");
    } else if (!strcmp(args, "ble")) {
        executor_set_transport(EXECUTOR_TRANSPORT_BLE);
        printf("Transport set to: ble\r\n");
    } else if (!strcmp(args, "usb")) {
        executor_set_transport(EXECUTOR_TRANSPORT_USB);
        printf("Transport set to: usb\r\n");
    } else {
        printf("Usage: transport <auto|ble|usb>\r\n");
    }
}

static void cmd_tap(char *args)
{
    while (*args == ' ') args++;
    if (!*args) {
        printf("Usage: tap <KEY_NAME> (e.g. tap KEY_A, tap KEY_ENTER)\r\n");
        return;
    }
    uint8_t code = 0, mods = 0;
    if (keymap_lookup(args, &code, &mods)) {
        if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) {
            hid_usb_tap(code, mods);
        } else {
            hid_ble_tap(code, mods);
        }
        printf("Tapped %s (code=0x%02X, mods=0x%02X)\r\n", args, code, mods);
    } else {
        printf("Unknown key: '%s'\r\n", args);
    }
}

static void cmd_move(char *args)
{
    while (*args == ' ') args++;
    int dx = 0, dy = 0;
    if (sscanf(args, "%d %d", &dx, &dy) == 2) {
        if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) {
            hid_usb_mouse_move((int8_t)dx, (int8_t)dy);
        } else {
            hid_ble_mouse_move((int8_t)dx, (int8_t)dy);
        }
        printf("Mouse moved dx=%d, dy=%d\r\n", dx, dy);
    } else {
        printf("Usage: move <dx> <dy>\r\n");
    }
}

static void cmd_click(char *args)
{
    while (*args == ' ') args++;
    int btn = 1;
    if (*args) btn = atoi(args);
    if (btn <= 0) btn = 1;
    if (executor_active_transport() == EXECUTOR_TRANSPORT_USB) {
        hid_usb_mouse_click((uint8_t)btn);
    } else {
        hid_ble_mouse_click((uint8_t)btn);
    }
    printf("Mouse clicked button=%d\r\n", btn);
}

static void cmd_text(char *args)
{
    while (*args == ' ') args++;
    if (!*args) {
        printf("Usage: text <string to type>\r\n");
        return;
    }
    bool is_usb = (executor_active_transport() == EXECUTOR_TRANSPORT_USB);
    for (const char *p = args; *p; ++p) {
        uint8_t code = 0, mods = 0;
        if (!keymap_ascii(*p, &code, &mods)) continue;
        if (is_usb) hid_usb_tap(code, mods);
        else        hid_ble_tap(code, mods);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("Typed: \"%s\"\r\n", args);
}

static void handle_command(char *line)
{
    while (*line == ' ') line++;
    if (!*line) return;

    // 分割命令与参数
    char *cmd = line;
    char *args = strchr(line, ' ');
    if (args) {
        *args = '\0';
        args++;
    } else {
        args = "";
    }

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        print_help();
    } else if (!strcmp(cmd, "status")) {
        cmd_status();
    } else if (!strcmp(cmd, "wifi")) {
        cmd_wifi(args);
    } else if (!strcmp(cmd, "wifi_scan")) {
        cmd_wifi_scan();
    } else if (!strcmp(cmd, "server")) {
        cmd_server(args);
    } else if (!strcmp(cmd, "transport")) {
        cmd_transport(args);
    } else if (!strcmp(cmd, "tap")) {
        cmd_tap(args);
    } else if (!strcmp(cmd, "text")) {
        cmd_text(args);
    } else if (!strcmp(cmd, "move")) {
        cmd_move(args);
    } else if (!strcmp(cmd, "click")) {
        cmd_click(args);
    } else if (!strcmp(cmd, "reboot")) {
        printf("Rebooting in 500ms...\r\n");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        printf("Unknown command: '%s'. Type 'help' for commands list.\r\n", cmd);
    }
}

static void cli_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));  // 等待系统各模块启动就绪
    print_banner();

    char buf[CLI_BUFFER_SIZE];
    int idx = 0;

    printf("esp32> ");
    fflush(stdout);

    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF || c < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (c == '\r' || c == '\n') {
            printf("\r\n");
            buf[idx] = '\0';
            if (idx > 0) {
                handle_command(buf);
                idx = 0;
            }
            printf("esp32> ");
            fflush(stdout);
        } else if (c == '\b' || c == 127) { // 退格
            if (idx > 0) {
                idx--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c >= 32 && c <= 126) {   // 可打印字符
            if (idx < CLI_BUFFER_SIZE - 1) {
                buf[idx++] = (char)c;
                fputc(c, stdout);
                fflush(stdout);
            }
        }
    }
}

esp_err_t serial_cli_start(void)
{
    BaseType_t ret = xTaskCreate(cli_task, "serial_cli", 4096, NULL, 3, NULL);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}
