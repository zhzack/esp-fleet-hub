#include "serial_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cmd_router.h"

/*
 * 单内核多前端（thin-CLI）架构：
 *   串口终端不再持有自己的命令实现，而是把 "cmd args..." 语法翻译成
 *   cmd_router 的 (type, JSON payload)，与云端 WS、LAN HTTP 前端共享同一份
 *   命令注册表、参数校验和错误码。内核侧的行为（如 transport.set 的 NVS
 *   持久化、wifi.connect 的失败回滚）只存在一份，所有前端自动一致。
 *   CLI 层职责仅剩：行编辑、语法翻译、结果展示。
 */

#define CLI_BUFFER_SIZE 256

static volatile bool s_activity = false;  /* 收到过完整命令行（OTA 健康自检兜底判据） */

bool serial_cli_activity(void) { return s_activity; }

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
    printf("  help                     : Show this help message\r\n");
    printf("  status                   : Device status (WiFi/BLE/USB/transport)\r\n");
    printf("  wifi <ssid> [password]   : Connect WiFi and save to NVS\r\n");
    printf("  wifi_scan                : Scan nearby access points (~3s)\r\n");
    printf("  server <host> [port]     : Set WebSocket server, saved to NVS\r\n");
    printf("  secret [<value>]         : Set/show device auth secret (NVS, no arg = show)\r\n");
    printf("  transport <auto|ble|usb> : Set HID transport (persisted to NVS)\r\n");
    printf("  tap <KEY_NAME>           : Tap a key (e.g. tap KEY_A)\r\n");
    printf("  text <string>            : Type text via HID\r\n");
    printf("  move <dx> <dy>           : Move mouse by delta (e.g. move 20 -10)\r\n");
    printf("  click [1|2|4]            : Click mouse button (1=left 2=right 4=mid)\r\n");
    printf("  ota [status|<url> [sha256]] : Show OTA status or upgrade (optional sha256)\r\n");
    printf("  reboot                   : Restart the ESP32\r\n\r\n");
}

/* ---------- dispatch helpers ---------- */

static const char *jstr(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int jnum(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
}

static int jbool(const cJSON *obj, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItem(obj, key));
}

/* 通用：dispatch 并打印 OK <json> / ERROR <rc>: <err> */
static int cmd_generic(const char *type, cJSON *payload)
{
    cJSON *result = cJSON_CreateObject();
    char err[96] = {0};
    int rc = cmd_router_dispatch(type, payload, result, err, sizeof err);
    if (rc != 0) {
        printf("ERROR %d: %s\r\n", rc, err);
    } else {
        char *s = cJSON_PrintUnformatted(result);
        if (s && s[0]) printf("OK %s\r\n", s);
        else           printf("OK\r\n");
        free(s);
    }
    cJSON_Delete(result);
    return rc;
}

/* HID 动作：走内核 "action" 通道，回显实际使用的传输层。
 * 注意：链路未就绪（E007 no USB host / no BLE peer）时内核会拒绝，
 * 这正是旧 CLI 直接调 hid_* 时被静默吞掉的情形。 */
static int cmd_hid(cJSON *payload)
{
    cJSON *result = cJSON_CreateObject();
    char err[96] = {0};
    int rc = cmd_router_dispatch("action", payload, result, err, sizeof err);
    if (rc != 0) {
        printf("ERROR %d: %s\r\n", rc, err);
    } else {
        const char *tr = jstr(result, "transport");
        printf("OK (transport=%s)\r\n", tr ? tr : "?");
    }
    cJSON_Delete(result);
    return rc;
}

/* ---------- 格式化展示的命令 ---------- */

static void cmd_status(void)
{
    cJSON *result = cJSON_CreateObject();
    char err[96] = {0};
    int rc = cmd_router_dispatch("get_status", NULL, result, err, sizeof err);
    if (rc != 0) {
        printf("ERROR %d: %s\r\n", rc, err);
        cJSON_Delete(result);
        return;
    }

    const cJSON *w = cJSON_GetObjectItem(result, "wifi");
    const cJSON *u = cJSON_GetObjectItem(result, "usb");
    const cJSON *b = cJSON_GetObjectItem(result, "ble");
    const char *did = jstr(result, "device_id");
    const char *fw  = jstr(result, "fw");
    const char *tm  = jstr(result, "transport_mode");
    const char *ta  = jstr(result, "transport");

    printf("\r\n[Device Status]\r\n");
    printf("  Device ID   : %s\r\n", did ? did : "unknown");
    printf("  Firmware    : %s\r\n", fw ? fw : "?");
    printf("  WiFi        : %s (SSID: %s, IP: %s, RSSI: %d dBm)\r\n",
           jbool(w, "connected") ? "CONNECTED" : "DISCONNECTED",
           jstr(w, "ssid") ? jstr(w, "ssid") : "-",
           jstr(w, "ip") ? jstr(w, "ip") : "-",
           jnum(w, "rssi"));
    printf("  USB         : %s\r\n", jbool(u, "mounted") ? "MOUNTED" : "UNMOUNTED");
    printf("  BLE         : %s\r\n", jbool(b, "connected") ? "CONNECTED" : "ADVERTISING");
    printf("  Transport   : mode=%s, active=%s\r\n", tm ? tm : "?", ta ? ta : "?");
    printf("  Free Heap   : %d bytes\r\n\r\n", jnum(result, "free_heap"));
    cJSON_Delete(result);
}

static void cmd_wifi_scan(void)
{
    printf("Scanning WiFi networks (takes ~3s)...\r\n");
    cJSON *result = cJSON_CreateObject();
    char err[96] = {0};
    int rc = cmd_router_dispatch("wifi.scan", NULL, result, err, sizeof err);
    if (rc != 0) {
        printf("ERROR %d: %s\r\n", rc, err);
        cJSON_Delete(result);
        return;
    }
    const cJSON *nets = cJSON_GetObjectItem(result, "networks");
    int n = cJSON_IsArray(nets) ? cJSON_GetArraySize(nets) : 0;
    printf("Found %d access points:\r\n", n);
    int i = 1;
    const cJSON *it;
    cJSON_ArrayForEach(it, nets) {
        const char *ssid = jstr(it, "ssid");
        printf("  [%02d] SSID: %-24s RSSI: %d dBm\r\n", i++, ssid ? ssid : "", jnum(it, "rssi"));
    }
    cJSON_Delete(result);
}

/* ---------- 语法翻译命令 ---------- */

static void cmd_wifi(char *args)
{
    while (*args == ' ') args++;
    if (!*args) {
        printf("Usage: wifi <ssid> [password]\r\n");
        return;
    }
    char *ssid = args, *pass = "";
    char *sep = strchr(args, ' ');
    if (sep) {
        *sep = '\0';
        pass = sep + 1;
        while (*pass == ' ') pass++;
    }
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "ssid", ssid);
    cJSON_AddStringToObject(pl, "pass", pass);
    cmd_generic("wifi.connect", pl);
    cJSON_Delete(pl);
}

static void cmd_server(char *args)
{
    while (*args == ' ') args++;
    if (!*args) {
        printf("Usage: server <host> [port]\r\n");
        return;
    }
    char *host = args;
    int port = 8000;
    char *sep = strchr(args, ' ');
    if (sep) {
        *sep = '\0';
        port = atoi(sep + 1);
        if (port <= 0) port = 8000;
    }
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "host", host);
    cJSON_AddNumberToObject(pl, "port", port);
    if (cmd_generic("server.set", pl) == 0) {
        printf("Saved. Reboot ('reboot') to connect to the new server.\r\n");
    }
    cJSON_Delete(pl);
}

static void cmd_transport(char *args)
{
    while (*args == ' ') args++;
    if (!*args) {
        printf("Usage: transport <auto|ble|usb>\r\n");
        return;
    }
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "mode", args);
    if (cmd_generic("transport.set", pl) == 0) {
        printf("Transport set to: %s (persisted)\r\n", args);
    }
    cJSON_Delete(pl);
}

static void cmd_secret(char *args)
{
    while (*args == ' ') args++;
    cJSON *pl = cJSON_CreateObject();
    if (*args) {
        cJSON_AddStringToObject(pl, "secret", args);
    }
    if (cmd_generic("auth.secret", pl) == 0 && *args) {
        printf("Secret saved. Reboot ('reboot') to reconnect with auth.\r\n");
    }
    cJSON_Delete(pl);
}

static void cmd_tap(char *args)
{
    while (*args == ' ') args++;
    if (!*args) {
        printf("Usage: tap <KEY_NAME> (e.g. tap KEY_A, tap KEY_ENTER)\r\n");
        return;
    }
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "type", "keyboard_tap");
    cJSON_AddStringToObject(pl, "key", args);
    cmd_hid(pl);
    cJSON_Delete(pl);
}

static void cmd_text(char *args)
{
    while (*args == ' ') args++;
    if (!*args) {
        printf("Usage: text <string to type>\r\n");
        return;
    }
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "type", "text");
    cJSON_AddStringToObject(pl, "content", args);
    cmd_hid(pl);
    cJSON_Delete(pl);
}

static void cmd_move(char *args)
{
    int dx = 0, dy = 0;
    if (sscanf(args, "%d %d", &dx, &dy) != 2) {
        printf("Usage: move <dx> <dy>\r\n");
        return;
    }
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "type", "mouse_move");
    cJSON_AddNumberToObject(pl, "x", dx);
    cJSON_AddNumberToObject(pl, "y", dy);
    cmd_hid(pl);
    cJSON_Delete(pl);
}

static void cmd_click(char *args)
{
    while (*args == ' ') args++;
    int btn = *args ? atoi(args) : 1;
    if (btn <= 0) btn = 1;
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "type", "mouse_click");
    cJSON_AddNumberToObject(pl, "button", btn);
    cmd_hid(pl);
    cJSON_Delete(pl);
}

static void cmd_ota(char *args)
{
    while (*args == ' ') args++;
    if (!*args || !strcmp(args, "status")) {
        cmd_generic("ota.status", NULL);
    } else {
        char *url = args;
        char *sep = strchr(args, ' ');
        char *sha = NULL;
        if (sep) {
            *sep = '\0';
            sha = sep + 1;
            while (*sha == ' ') sha++;
        }
        cJSON *pl = cJSON_CreateObject();
        cJSON_AddStringToObject(pl, "url", url);
        if (sha && *sha) {
            cJSON_AddStringToObject(pl, "sha256", sha);
        }
        if (cmd_generic("ota.start", pl) == 0) {
            printf("OTA task started. Use 'ota status' to view progress.\r\n");
        }
        cJSON_Delete(pl);
    }
}

static void handle_command(char *line)
{
    while (*line == ' ') line++;
    if (!*line) return;

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
    } else if (!strcmp(cmd, "secret")) {
        cmd_secret(args);
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
    } else if (!strcmp(cmd, "ota")) {
        cmd_ota(args);
    } else if (!strcmp(cmd, "reboot")) {
        cmd_generic("reboot", NULL);
    } else {
        printf("Unknown command: '%s'. Type 'help' for commands list.\r\n", cmd);
    }
}

static void cli_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));  /* 等待系统各模块启动就绪（cmd_router_init 等） */
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
                s_activity = true;  /* OTA 健康自检兜底判据 */
                handle_command(buf);
                idx = 0;
            }
            printf("esp32> ");
            fflush(stdout);
        } else if (c == '\b' || c == 127) { /* 退格 */
            if (idx > 0) {
                idx--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c >= 32 && c <= 126) {   /* 可打印字符 */
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
