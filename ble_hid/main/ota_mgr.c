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

#include "app_config.h"
#include "protocol.h"
#include "ws_client.h"

static const char *TAG = "ota_mgr";

#define OTA_BUFF_SIZE 2048

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

    // Cancel rollback so this boot is marked successful
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_LOGI(TAG, "OTA Initialized. Running partition: %s, Next update partition: %s, Version: %s",
             s_info.running_partition, s_info.target_partition, s_info.version);
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
    if (s_info.err_msg[0]) {
        cJSON_AddStringToObject(dst, "error", s_info.err_msg);
    }
}
