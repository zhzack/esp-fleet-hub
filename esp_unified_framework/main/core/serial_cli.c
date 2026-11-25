#include "serial_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "system_caps.h"
#include "wifi_mgr.h"
#if CONFIG_FEATURE_GPIO_CTRL
#include "hal_gpio.h"
#endif

static const char *TAG = "cli";

#if CONFIG_FEATURE_SERIAL_CLI

static void print_help(void)
{
    printf("\n=== ESP Unified Serial CLI ===\n"
           "  help                 - Show commands\n"
           "  status               - Show device ID, chip, WiFi, Hub & memory\n"
           "  wifi <ssid> [pass]   - Set & switch WiFi (15s auto-rollback guard)\n"
           "  hub <url|none>       - Set Fleet Hub URL ('none' = auto mDNS)\n"
           "  id <device_id>       - Set device ID\n"
           "  mdns <host>          - Set mDNS hostname\n"
           "  secret <val|none>    - Set HMAC registration secret\n"
           "  token <val|none>     - Set HTTP/REST API token\n"
           "  ap <on|off>          - Start/Stop SoftAP Captive Portal\n"
           "  gpio [<pin> <0|1>]   - List GPIOs or write pin level\n"
           "  reset                - Factory reset NVS 'app' & reboot\n"
           "  reboot               - Reboot device\n"
           "==============================\n");
}

static void handle_line(char *line)
{
    // Strip trailing whitespace
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        printf("esp> ");
        fflush(stdout);
        return;
    }

    char *argv[4] = {0};
    int argc = 0;
    char *tok = strtok(line, " \t");
    while (tok && argc < 4) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    if (argc == 0) return;

    const char *cmd = argv[0];
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_help();
    } else if (strcmp(cmd, "status") == 0) {
        app_config_t cfg;
        app_config_get(&cfg);
        system_caps_t caps;
        system_caps_get(&caps);
        printf("Device ID : %s\n"
               "Chip/Board: %s | %s\n"
               "FW Version: %s (cfg_ver=%lu)\n"
               "MAC / IP  : %s | %s (AP=%s)\n"
               "WiFi SSID : %s (%d dBm, conn=%s)\n"
               "mDNS Host : http://%s.local:%u\n"
               "Fleet Hub : %s\n"
               "Security  : secret=%s | api_token=%s\n"
               "Memory    : Heap=%lu B | PSRAM=%lu B | Flash=%lu MB\n",
               cfg.device_id, caps.chip_name, caps.board_name, caps.fw_version,
               (unsigned long)cfg.config_ver, caps.mac_addr, caps.ip_addr,
               wifi_mgr_is_ap_active() ? "ON" : "OFF",
               caps.wifi_ssid, caps.wifi_rssi, wifi_mgr_connected() ? "YES" : "NO",
               cfg.mdns_host, cfg.web_port,
               cfg.hub_url[0] ? cfg.hub_url : "(auto mDNS)",
               cfg.device_secret[0] ? "SET" : "NONE",
               cfg.api_token[0] ? "SET" : "OPEN",
               (unsigned long)caps.free_heap, (unsigned long)caps.free_psram,
               (unsigned long)caps.flash_size_mb);
    } else if (strcmp(cmd, "wifi") == 0 && argc >= 2) {
        const char *ssid = argv[1];
        const char *pass = (argc >= 3) ? argv[2] : "";
        app_config_t cfg;
        app_config_get(&cfg);
        app_config_backup_wifi(cfg.wifi_ssid, cfg.wifi_pass);
        strlcpy(cfg.wifi_ssid, ssid, sizeof(cfg.wifi_ssid));
        strlcpy(cfg.wifi_pass, pass, sizeof(cfg.wifi_pass));
        cfg.config_ver++;
        app_config_save(&cfg);
        wifi_mgr_apply_new_sta(ssid, pass);
        printf("OK: switching WiFi to '%s' (15s rollback guard active)\n", ssid);
    } else if (strcmp(cmd, "hub") == 0 && argc >= 2) {
        app_config_t cfg;
        app_config_get(&cfg);
        if (strcmp(argv[1], "none") == 0) cfg.hub_url[0] = '\0';
        else strlcpy(cfg.hub_url, argv[1], sizeof(cfg.hub_url));
        cfg.config_ver++;
        app_config_save(&cfg);
        printf("OK: hub_url='%s'\n", cfg.hub_url[0] ? cfg.hub_url : "(auto mDNS)");
    } else if (strcmp(cmd, "id") == 0 && argc >= 2) {
        app_config_t cfg;
        app_config_get(&cfg);
        strlcpy(cfg.device_id, argv[1], sizeof(cfg.device_id));
        cfg.config_ver++;
        app_config_save(&cfg);
        wifi_mgr_update_mdns(cfg.mdns_host, cfg.device_id, cfg.web_port);
        printf("OK: device_id='%s'\n", cfg.device_id);
    } else if (strcmp(cmd, "mdns") == 0 && argc >= 2) {
        app_config_t cfg;
        app_config_get(&cfg);
        strlcpy(cfg.mdns_host, argv[1], sizeof(cfg.mdns_host));
        cfg.config_ver++;
        app_config_save(&cfg);
        wifi_mgr_update_mdns(cfg.mdns_host, cfg.device_id, cfg.web_port);
        printf("OK: mdns_host='%s'\n", cfg.mdns_host);
    } else if (strcmp(cmd, "secret") == 0 && argc >= 2) {
        app_config_t cfg;
        app_config_get(&cfg);
        if (strcmp(argv[1], "none") == 0) cfg.device_secret[0] = '\0';
        else strlcpy(cfg.device_secret, argv[1], sizeof(cfg.device_secret));
        cfg.config_ver++;
        app_config_save(&cfg);
        printf("OK: device_secret %s\n", cfg.device_secret[0] ? "updated" : "cleared");
    } else if (strcmp(cmd, "token") == 0 && argc >= 2) {
        app_config_t cfg;
        app_config_get(&cfg);
        if (strcmp(argv[1], "none") == 0) cfg.api_token[0] = '\0';
        else strlcpy(cfg.api_token, argv[1], sizeof(cfg.api_token));
        cfg.config_ver++;
        app_config_save(&cfg);
        printf("OK: api_token %s\n", cfg.api_token[0] ? "enabled" : "disabled");
    } else if (strcmp(cmd, "ap") == 0 && argc >= 2) {
        if (strcmp(argv[1], "on") == 0) {
            wifi_mgr_start_softap();
            printf("OK: SoftAP started at 192.168.4.1\n");
        } else {
            wifi_mgr_stop_softap();
            printf("OK: SoftAP stopped\n");
        }
    } else if (strcmp(cmd, "gpio") == 0) {
#if CONFIG_FEATURE_GPIO_CTRL
        if (argc == 1) {
            size_t cnt = 0;
            const gpio_pin_desc_t *pins = hal_gpio_get_pins(&cnt);
            for (size_t i = 0; i < cnt; i++) {
                printf("  GPIO %-2d (%s): val=%d duty=%u%% pwm=%d\n",
                       pins[i].pin_num, pins[i].label, hal_gpio_read(pins[i].pin_num),
                       pins[i].current_duty, pins[i].supports_pwm);
            }
        } else if (argc >= 3) {
            int pin = atoi(argv[1]);
            int val = atoi(argv[2]);
            esp_err_t e = hal_gpio_write(pin, val);
            printf("GPIO %d -> %d (%s)\n", pin, val, esp_err_to_name(e));
        }
#else
        printf("GPIO module not compiled.\n");
#endif
    } else if (strcmp(cmd, "reset") == 0) {
        printf("Factory resetting NVS & rebooting...\n");
        app_config_factory_reset();
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else if (strcmp(cmd, "reboot") == 0) {
        printf("Rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        printf("Unknown or incomplete command: '%s'. Type 'help'.\n", cmd);
    }

    printf("esp> ");
    fflush(stdout);
}

static void serial_cli_task(void *pvParameters)
{
    char line_buf[160];
    size_t pos = 0;

    // Drain any boot garbage and prompt cleanly once
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Serial CLI ready. Type 'help' + Enter for interactive configuration.");

    while (1) {
        int c = fgetc(stdin);
        if (c == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (c == '\r' || c == '\n') {
            putchar('\n');
            line_buf[pos] = '\0';
            handle_line(line_buf);
            pos = 0;
        } else if (c == '\b' || c == 0x7F) {
            if (pos > 0) {
                pos--;
                fputs("\b \b", stdout);
                fflush(stdout);
            }
        } else if (c >= 0x20 && c < 0x7F && pos < sizeof(line_buf) - 1) {
            line_buf[pos++] = (char)c;
            putchar(c);
            fflush(stdout);
        }
    }
}

esp_err_t serial_cli_init(void)
{
    BaseType_t ok = xTaskCreate(serial_cli_task, "serial_cli", 4096, NULL, 2, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

#else
esp_err_t serial_cli_init(void) { return ESP_OK; }
#endif
