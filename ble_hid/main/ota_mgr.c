#include "ota_mgr.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_app_format.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "hid_usb.h"
#include "protocol.h"
#include "serial_cli.h"
#include "wifi_mgr.h"
#include "ws_client.h"

static const char *TAG = "ota_mgr";

#define OTA_BUFF_SIZE 2048

/* 健康自检参数：OTA 首启后最长等待 90s；健康判据（WS 在线或串口有活动）
 * 需连续保持 5s 才 mark valid，避免瞬间抖动误判。 */
#define OTA_HEALTH_TIMEOUT_S   90
#define OTA_HEALTH_STABLE_S    5

typedef struct {
    char url[256];
    char version[32];
    char sha256[65];
} ota_task_params_t;

static ota_info_t s_info = {
    .status = OTA_STATUS_IDLE,
    .progress = 0,
    .bytes_read = 0,
    .total_bytes = 0,
    .err_msg = "",
    .running_partition = "unknown",
    .target_partition = "unknown",
    .version = "unknown",
};

static TaskHandle_t s_ota_task_handle = NULL;
static ota_task_params_t s_params;

static void send_ws_progress(const char *status_str, int progress, int read_bytes, int total_bytes, const char *err)
{
    cJSON *env = cJSON_CreateObject();
    if (!env) return;

    cJSON *hdr = cJSON_AddObjectToObject(env, "header");
    char uuid[37];
    protocol_make_uuid(uuid);
    cJSON_AddStringToObject(hdr, "msg_id", uuid);
    cJSON_AddStringToObject(hdr, "type", "ota_progress");
    cJSON_AddStringToObject(hdr, "device_id", app_config_get()->device_id);
    cJSON_AddNumberToObject(hdr, "timestamp", (double)time(NULL));
    cJSON_AddStringToObject(hdr, "source", "esp32");

    cJSON_AddObjectToObject(env, "routing");

    cJSON *p = cJSON_AddObjectToObject(env, "payload");
    cJSON_AddStringToObject(p, "status", status_str);
    cJSON_AddNumberToObject(p, "progress", progress);
    cJSON_AddNumberToObject(p, "bytes_read", read_bytes);
    cJSON_AddNumberToObject(p, "total_bytes", total_bytes);
    cJSON_AddStringToObject(p, "running_partition", s_info.running_partition);
    cJSON_AddStringToObject(p, "target_partition", s_info.target_partition);
    if (err && err[0]) {
        cJSON_AddStringToObject(p, "error", err);
    }

    if (ws_client_send(env) != ESP_OK) {
        cJSON_Delete(env);
    }
}

static void ota_task(void *pvParameter)
{
    ota_task_params_t *params = (ota_task_params_t *)pvParameter;
    ESP_LOGI(TAG, "OTA Task started, fetching: %s", params->url);

    s_info.status = OTA_STATUS_DOWNLOADING;
    s_info.progress = 0;
    s_info.bytes_read = 0;
    s_info.total_bytes = 0;
    s_info.err_msg[0] = '\0';
    if (params->version[0]) {
        strlcpy(s_info.version, params->version, sizeof(s_info.version));
    }

    send_ws_progress("connecting", 0, 0, 0, NULL);

    esp_http_client_config_t http_config = {
        .url = params->url,
        .timeout_ms = 25000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        snprintf(s_info.err_msg, sizeof(s_info.err_msg), "Failed to init HTTP client");
        s_info.status = OTA_STATUS_FAILED;
        ESP_LOGE(TAG, "%s", s_info.err_msg);
        send_ws_progress("failed", 0, 0, 0, s_info.err_msg);
        s_ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(s_info.err_msg, sizeof(s_info.err_msg), "Failed to open HTTP connection: 0x%x", err);
        s_info.status = OTA_STATUS_FAILED;
        ESP_LOGE(TAG, "%s", s_info.err_msg);
        esp_http_client_cleanup(client);
        send_ws_progress("failed", 0, 0, 0, s_info.err_msg);
        s_ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 300) {
        snprintf(s_info.err_msg, sizeof(s_info.err_msg), "HTTP error status: %d", status_code);
        s_info.status = OTA_STATUS_FAILED;
        ESP_LOGE(TAG, "%s", s_info.err_msg);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        send_ws_progress("failed", 0, 0, 0, s_info.err_msg);
        s_ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_info.total_bytes = content_length;
    ESP_LOGI(TAG, "Firmware size: %d bytes (HTTP %d)", content_length, status_code);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        snprintf(s_info.err_msg, sizeof(s_info.err_msg), "No update partition available");
        s_info.status = OTA_STATUS_FAILED;
        ESP_LOGE(TAG, "%s", s_info.err_msg);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        send_ws_progress("failed", 0, 0, 0, s_info.err_msg);
        s_ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    strlcpy(s_info.target_partition, update_partition->label, sizeof(s_info.target_partition));
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%" PRIx32 " (label %s)",
             update_partition->subtype, update_partition->address, update_partition->label);

    esp_ota_handle_t update_handle = 0;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK) {
        snprintf(s_info.err_msg, sizeof(s_info.err_msg), "esp_ota_begin failed: 0x%x", err);
        s_info.status = OTA_STATUS_FAILED;
        ESP_LOGE(TAG, "%s", s_info.err_msg);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        send_ws_progress("failed", 0, 0, 0, s_info.err_msg);
        s_ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    char *ota_write_data = malloc(OTA_BUFF_SIZE);
    if (!ota_write_data) {
        snprintf(s_info.err_msg, sizeof(s_info.err_msg), "Out of memory for OTA buffer");
        s_info.status = OTA_STATUS_FAILED;
        ESP_LOGE(TAG, "%s", s_info.err_msg);
        esp_ota_abort(update_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        send_ws_progress("failed", 0, 0, 0, s_info.err_msg);
        s_ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    int binary_file_len = 0;
    int last_reported_pct = -1;

    /* 期望 sha256 非空时对下载流做完整性校验（后端 upload 时计算的文件哈希） */
    const bool want_sha = params->sha256[0] != '\0';
    mbedtls_sha256_context sha_ctx;
    if (want_sha) {
        mbedtls_sha256_init(&sha_ctx);
        mbedtls_sha256_starts(&sha_ctx, 0);
    }

    while (1) {
        int data_read = esp_http_client_read(client, ota_write_data, OTA_BUFF_SIZE);
        if (data_read < 0) {
            snprintf(s_info.err_msg, sizeof(s_info.err_msg), "HTTP read error");
            s_info.status = OTA_STATUS_FAILED;
            break;
        } else if (data_read > 0) {
            err = esp_ota_write(update_handle, (const void *)ota_write_data, data_read);
            if (err != ESP_OK) {
                snprintf(s_info.err_msg, sizeof(s_info.err_msg), "esp_ota_write failed: 0x%x", err);
                s_info.status = OTA_STATUS_FAILED;
                break;
            }
            if (want_sha) {
                mbedtls_sha256_update(&sha_ctx, (const unsigned char *)ota_write_data, data_read);
            }
            binary_file_len += data_read;
            s_info.bytes_read = binary_file_len;

            int pct = 0;
            if (content_length > 0) {
                pct = (int)(((int64_t)binary_file_len * 100) / content_length);
            }
            if (pct > 100) pct = 100;
            s_info.progress = pct;

            if (pct >= last_reported_pct + 5 || pct == 100) {
                last_reported_pct = pct;
                ESP_LOGI(TAG, "OTA progress: %d%% (%d / %d bytes)", pct, binary_file_len, content_length);
                send_ws_progress("downloading", pct, binary_file_len, content_length, NULL);
            }
        } else if (data_read == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                ESP_LOGI(TAG, "All data received successfully (%d bytes)", binary_file_len);
                break;
            }
            // In chunked transfer without Content-Length, 0 bytes read marks EOF
            if (content_length <= 0 && binary_file_len > 0) {
                ESP_LOGI(TAG, "Chunked transfer EOF reached (%d bytes)", binary_file_len);
                break;
            }
            break;
        }
    }

    free(ota_write_data);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (s_info.status == OTA_STATUS_FAILED) {
        ESP_LOGE(TAG, "OTA failed: %s", s_info.err_msg);
        esp_ota_abort(update_handle);
        send_ws_progress("failed", s_info.progress, binary_file_len, content_length, s_info.err_msg);
        s_ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        snprintf(s_info.err_msg, sizeof(s_info.err_msg), "esp_ota_end failed: 0x%x", err);
        s_info.status = OTA_STATUS_FAILED;
        ESP_LOGE(TAG, "%s", s_info.err_msg);
        send_ws_progress("failed", s_info.progress, binary_file_len, content_length, s_info.err_msg);
        s_ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* 下载流 sha256 与期望值比对：不匹配则不切换启动分区，当前固件继续运行 */
    if (want_sha) {
        uint8_t digest[32];
        mbedtls_sha256_finish(&sha_ctx, digest);
        mbedtls_sha256_free(&sha_ctx);
        static const char hex[] = "0123456789abcdef";
        char actual[65];
        for (int i = 0; i < 32; i++) {
            actual[2 * i]     = hex[digest[i] >> 4];
            actual[2 * i + 1] = hex[digest[i] & 0xF];
        }
        actual[64] = '\0';
        if (strcmp(actual, params->sha256) != 0) {
            snprintf(s_info.err_msg, sizeof(s_info.err_msg),
                     "sha256 mismatch (expected %.16s..., got %.16s...)", params->sha256, actual);
            s_info.status = OTA_STATUS_FAILED;
            ESP_LOGE(TAG, "%s", s_info.err_msg);
            send_ws_progress("failed", 100, binary_file_len, content_length, s_info.err_msg);
            s_ota_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "sha256 verified OK (%.16s...)", actual);
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        snprintf(s_info.err_msg, sizeof(s_info.err_msg), "esp_ota_set_boot_partition failed: 0x%x", err);
        s_info.status = OTA_STATUS_FAILED;
        ESP_LOGE(TAG, "%s", s_info.err_msg);
        send_ws_progress("failed", s_info.progress, binary_file_len, content_length, s_info.err_msg);
        s_ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_info.status = OTA_STATUS_SUCCESS;
    s_info.progress = 100;
    ESP_LOGI(TAG, "OTA Success! Next boot partition: %s. Restarting in 2 seconds...", update_partition->label);
    send_ws_progress("success", 100, binary_file_len, binary_file_len, NULL);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    s_ota_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ---------- OTA 健康自检（新分区 PENDING_VERIFY 期间） ----------
 * 背景：CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y 时，OTA 升级后的首次启动
 * 分区处于 ESP_OTA_IMG_PENDING_VERIFY——必须显式 mark valid，否则下一次
 * 复位（包括 crash / 看门狗）bootloader 会自动回滚旧分区。
 * 策略：不再无条件取消回滚，改为等待"健康判据"成立后再确认：
 *   健康 = WS 注册在线（Wi-Fi + TCP + WS + cJSON 全链路工作）
 *          或 串口 CLI 有过命令（有人在场，设备可交互）
 * 判据需连续保持 OTA_HEALTH_STABLE_S 秒；超过 OTA_HEALTH_TIMEOUT_S 仍不
 * 健康则主动回滚旧分区重启。配置 APP_OTA_TEST_UNHEALTHY=y 可强制不健康
 * （真机回滚链路验证用）。 */
static bool health_ok(void)
{
#ifdef CONFIG_APP_OTA_TEST_UNHEALTHY
    return false;
#else
    return wifi_mgr_connected() || hid_usb_mounted() || ws_client_online() || serial_cli_activity();
#endif
}

static void ota_health_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "running partition is PENDING_VERIFY — health check started "
                  "(stable %ds, timeout %ds)", OTA_HEALTH_STABLE_S, OTA_HEALTH_TIMEOUT_S);
    int stable = 0;
    for (int t = 0; t < OTA_HEALTH_TIMEOUT_S; t++) {
        if (health_ok()) {
            if (++stable >= OTA_HEALTH_STABLE_S) {
                esp_ota_mark_app_valid_cancel_rollback();
                s_info.health = OTA_HEALTH_PASSED;
                ESP_LOGI(TAG, "health check PASSED (%ds) — app marked valid, rollback cancelled", t);
                vTaskDelete(NULL);
                return;
            }
        } else {
            stable = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    s_info.health = OTA_HEALTH_ROLLBACK_TIMEOUT;
    ESP_LOGE(TAG, "health check TIMEOUT after %ds — rolling back to previous partition",
             OTA_HEALTH_TIMEOUT_S);
    esp_ota_mark_app_invalid_rollback_and_reboot();  /* no return */
}

void ota_mgr_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        strlcpy(s_info.running_partition, running->label, sizeof(s_info.running_partition));
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next) {
        strlcpy(s_info.target_partition, next->label, sizeof(s_info.target_partition));
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc) {
        strlcpy(s_info.version, app_desc->version, sizeof(s_info.version));
    }

    /* OTA 首启检测：PENDING_VERIFY 才需要健康自检；其余（线刷 / 已确认）直接有效 */
    esp_ota_img_states_t state = ESP_OTA_IMG_VALID;
    esp_err_t st = running ? esp_ota_get_state_partition(running, &state) : ESP_ERR_INVALID_ARG;
    if (st == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_info.health = OTA_HEALTH_MONITORING;
        xTaskCreate(ota_health_task, "ota_health", 3072, NULL, 4, NULL);
    } else {
        s_info.health = OTA_HEALTH_NOT_NEEDED;
        /* 非 PENDING_VERIFY 的一次普通启动，取消回滚标记属幂等空操作 */
        esp_ota_mark_app_valid_cancel_rollback();
    }

    ESP_LOGI(TAG, "OTA Initialized. Running partition: %s, Next update partition: %s, Version: %s, img_state: %d",
             s_info.running_partition, s_info.target_partition, s_info.version, (int)state);
}

esp_err_t ota_mgr_start(const char *url, const char *version, const char *sha256)
{
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_info.status == OTA_STATUS_DOWNLOADING || s_info.status == OTA_STATUS_STARTING) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_params, 0, sizeof(s_params));
    strlcpy(s_params.url, url, sizeof(s_params.url));
    if (version) strlcpy(s_params.version, version, sizeof(s_params.version));
    if (sha256)  strlcpy(s_params.sha256, sha256, sizeof(s_params.sha256));

    s_info.status = OTA_STATUS_STARTING;
    s_info.progress = 0;
    s_info.err_msg[0] = '\0';

    BaseType_t ret = xTaskCreate(ota_task, "ota_task", 8192, &s_params, 5, &s_ota_task_handle);
    if (ret != pdPASS) {
        s_info.status = OTA_STATUS_FAILED;
        snprintf(s_info.err_msg, sizeof(s_info.err_msg), "Failed to create FreeRTOS task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void ota_mgr_get_info(ota_info_t *out)
{
    if (out) {
        *out = s_info;
    }
}

void ota_mgr_build_json(cJSON *dst)
{
    if (!dst) return;
    const char *st_str = "idle";
    switch (s_info.status) {
        case OTA_STATUS_STARTING:    st_str = "starting"; break;
        case OTA_STATUS_DOWNLOADING: st_str = "downloading"; break;
        case OTA_STATUS_SUCCESS:     st_str = "success"; break;
        case OTA_STATUS_FAILED:      st_str = "failed"; break;
        default:                     st_str = "idle"; break;
    }

    cJSON_AddStringToObject(dst, "status", st_str);
    cJSON_AddNumberToObject(dst, "progress", s_info.progress);
    cJSON_AddNumberToObject(dst, "bytes_read", s_info.bytes_read);
    cJSON_AddNumberToObject(dst, "total_bytes", s_info.total_bytes);
    cJSON_AddStringToObject(dst, "running_partition", s_info.running_partition);
    cJSON_AddStringToObject(dst, "target_partition", s_info.target_partition);
    cJSON_AddStringToObject(dst, "current_version", s_info.version);

    /* 回滚 / 健康自检状态（远程诊断与真机验证用） */
    const char *health_str = "not_needed";
    switch (s_info.health) {
        case OTA_HEALTH_MONITORING:       health_str = "monitoring"; break;
        case OTA_HEALTH_PASSED:           health_str = "passed"; break;
        case OTA_HEALTH_ROLLBACK_TIMEOUT: health_str = "rollback_timeout"; break;
        default:                          health_str = "not_needed"; break;
    }
    cJSON_AddStringToObject(dst, "health_check", health_str);
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t st2 = ESP_OTA_IMG_INVALID;
        const char *rb = "unknown";
        if (running && esp_ota_get_state_partition(running, &st2) == ESP_OK) {
            switch (st2) {
                case ESP_OTA_IMG_VALID:          rb = "valid"; break;
                case ESP_OTA_IMG_PENDING_VERIFY: rb = "pending_verify"; break;
                default:                         rb = "other"; break;
            }
        }
        cJSON_AddStringToObject(dst, "rollback_state", rb);
    }
    if (s_info.err_msg[0]) {
        cJSON_AddStringToObject(dst, "error", s_info.err_msg);
    }
}
