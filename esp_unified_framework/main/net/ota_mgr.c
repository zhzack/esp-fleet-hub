#include "ota_mgr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "cJSON.h"
#include "wifi_mgr.h"

static const char *TAG = "ota_mgr";

static volatile bool s_ota_in_progress = false;
static volatile bool s_pending_verify = false;
static char s_last_sha256[65] = "";
static char s_last_error[96] = "";
static ota_progress_cb_t s_progress_cb = NULL;

void ota_mgr_set_progress_cb(ota_progress_cb_t cb)
{
    s_progress_cb = cb;
}

static void emit_progress(const char *status, int pct, int bytes_read, int total_bytes, const char *err)
{
    if (s_progress_cb) {
        s_progress_cb(status, pct, bytes_read, total_bytes, err ? err : "");
    }
}

typedef struct {
    char url[256];
    char sha256[65];
} ota_pull_job_t;

static void sha256_to_hex(const uint8_t digest[32], char out_hex[65])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[2 * i]     = hex[digest[i] >> 4];
        out_hex[2 * i + 1] = hex[digest[i] & 0x0F];
    }
    out_hex[64] = '\0';
}

static bool is_hex64(const char *s)
{
    if (!s || strlen(s) != 64) return false;
    for (int i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static const char *ota_state_str(esp_ota_img_states_t st)
{
    switch (st) {
        case ESP_OTA_IMG_NEW:            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID:          return "valid";
        case ESP_OTA_IMG_INVALID:        return "invalid";
        case ESP_OTA_IMG_ABORTED:        return "aborted";
        default:                         return "undefined";
    }
}

static void reboot_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Rebooting system in 1.5s after OTA flash...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void ota_health_verify_task(void *pvParameters)
{
    ESP_LOGW(TAG, "[OTA Self-Check] Image in PENDING_VERIFY state. Monitoring health up to 90s...");
    int stable_sec = 0;
    for (int sec = 0; sec < 90; sec++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (wifi_mgr_connected() && esp_get_free_heap_size() > 16384) {
            stable_sec++;
            if (stable_sec >= 5) {
                esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
                if (err == ESP_OK) {
                    s_pending_verify = false;
                    ESP_LOGI(TAG, "[OTA Self-Check] PASSED (WiFi+Heap stable 5s). Image marked VALID!");
                } else {
                    ESP_LOGE(TAG, "[OTA Self-Check] Failed to mark valid: %s", esp_err_to_name(err));
                }
                vTaskDelete(NULL);
                return;
            }
        } else {
            stable_sec = 0;
        }
    }

    ESP_LOGE(TAG, "[OTA Self-Check] FAILED after 90s! Rolling back to previous partition and rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_ota_mark_app_invalid_rollback_and_reboot();
    vTaskDelete(NULL);
}

esp_err_t ota_mgr_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        ESP_LOGI(TAG, "Running partition: %s (offset 0x%08lx, size 0x%08lx, state=%s)",
                 running->label, running->address, running->size, ota_state_str(state));
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            s_pending_verify = true;
            xTaskCreate(ota_health_verify_task, "ota_verify", 3072, NULL, 4, NULL);
        }
    } else {
        ESP_LOGI(TAG, "Running partition: %s (offset 0x%08lx, size 0x%08lx)",
                 running ? running->label : "unknown",
                 running ? running->address : 0,
                 running ? running->size : 0);
    }
    return ESP_OK;
}

size_t ota_mgr_status_to_json(char *dst, size_t max_len)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t app_desc = {0};
    if (running) esp_ota_get_partition_description(running, &app_desc);

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running) esp_ota_get_state_partition(running, &state);

    return snprintf(dst, max_len,
        "{"
          "\"ok\":true,"
          "\"running_partition\":\"%s\","
          "\"next_partition\":\"%s\","
          "\"rollback_state\":\"%s\","
          "\"pending_verify\":%s,"
          "\"ota_in_progress\":%s,"
          "\"last_sha256\":\"%s\","
          "\"last_error\":\"%s\","
          "\"version\":\"%s\","
          "\"project_name\":\"%s\","
          "\"idf_ver\":\"%s\","
          "\"compile_time\":\"%s %s\""
        "}",
        running ? running->label : "unknown",
        next ? next->label : "none",
        ota_state_str(state),
        s_pending_verify ? "true" : "false",
        s_ota_in_progress ? "true" : "false",
        s_last_sha256,
        s_last_error,
        app_desc.version,
        app_desc.project_name,
        app_desc.idf_ver,
        app_desc.date, app_desc.time
    );
}

esp_err_t ota_upload_post_handler(httpd_req_t *req)
{
    if (s_ota_in_progress) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA already in progress");
        return ESP_FAIL;
    }

    char expected_sha256[68] = "";
    if (httpd_req_get_hdr_value_len(req, "X-Firmware-SHA256") > 0) {
        httpd_req_get_hdr_value_str(req, "X-Firmware-SHA256", expected_sha256, sizeof(expected_sha256));
        if (expected_sha256[0] != '\0' && !is_hex64(expected_sha256)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid X-Firmware-SHA256 header");
            return ESP_FAIL;
        }
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "Passive OTA partition not found!");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA partition not found");
        return ESP_FAIL;
    }

    int content_len = req->content_len;
    if (content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Length required");
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    s_ota_in_progress = true;
    s_last_error[0] = '\0';

    ESP_LOGI(TAG, "Writing Push OTA (%d B) to partition: %s (0x%08lx)",
             content_len, update_partition->label, update_partition->address);

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    esp_ota_handle_t ota_handle = 0;
    int received = 0;
    int remaining = content_len;
    bool is_first_chunk = true;
    int timeout_retries = 0;
    esp_err_t err = ESP_OK;

    while (remaining > 0) {
        int to_read = remaining < 4096 ? remaining : 4096;
        int read_bytes = httpd_req_recv(req, buf, to_read);
        if (read_bytes <= 0) {
            if (read_bytes == HTTPD_SOCK_ERR_TIMEOUT && ++timeout_retries < 6) {
                continue;
            }
            snprintf(s_last_error, sizeof(s_last_error), "recv error %d", read_bytes);
            ESP_LOGE(TAG, "%s", s_last_error);
            free(buf);
            mbedtls_sha256_free(&sha_ctx);
            if (ota_handle) esp_ota_abort(ota_handle);
            s_ota_in_progress = false;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Stream recv failed");
            return ESP_FAIL;
        }
        timeout_retries = 0;

        if (is_first_chunk) {
            is_first_chunk = false;
            if ((uint8_t)buf[0] != 0xE9) {
                snprintf(s_last_error, sizeof(s_last_error), "bad magic 0x%02X", (uint8_t)buf[0]);
                ESP_LOGE(TAG, "%s", s_last_error);
                free(buf);
                mbedtls_sha256_free(&sha_ctx);
                s_ota_in_progress = false;
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware image header");
                return ESP_FAIL;
            }

            err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
            if (err != ESP_OK) {
                snprintf(s_last_error, sizeof(s_last_error), "ota_begin: %s", esp_err_to_name(err));
                free(buf);
                mbedtls_sha256_free(&sha_ctx);
                s_ota_in_progress = false;
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, s_last_error);
                return ESP_FAIL;
            }
        }

        mbedtls_sha256_update(&sha_ctx, (const unsigned char *)buf, read_bytes);
        err = esp_ota_write(ota_handle, buf, read_bytes);
        if (err != ESP_OK) {
            snprintf(s_last_error, sizeof(s_last_error), "ota_write: %s", esp_err_to_name(err));
            free(buf);
            mbedtls_sha256_free(&sha_ctx);
            esp_ota_abort(ota_handle);
            s_ota_in_progress = false;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, s_last_error);
            return ESP_FAIL;
        }

        received += read_bytes;
        remaining -= read_bytes;
    }

    free(buf);

    uint8_t digest[32];
    char actual_hex[65];
    mbedtls_sha256_finish(&sha_ctx, digest);
    mbedtls_sha256_free(&sha_ctx);
    sha256_to_hex(digest, actual_hex);

    if (expected_sha256[0] != '\0' && strcasecmp(expected_sha256, actual_hex) != 0) {
        snprintf(s_last_error, sizeof(s_last_error), "SHA-256 mismatch");
        ESP_LOGE(TAG, "Push OTA SHA-256 mismatch! expected=%s got=%s", expected_sha256, actual_hex);
        esp_ota_abort(ota_handle);
        s_ota_in_progress = false;
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware SHA-256 verification failed");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        snprintf(s_last_error, sizeof(s_last_error), "ota_end: %s", esp_err_to_name(err));
        s_ota_in_progress = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, s_last_error);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        snprintf(s_last_error, sizeof(s_last_error), "set_boot: %s", esp_err_to_name(err));
        s_ota_in_progress = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, s_last_error);
        return ESP_FAIL;
    }

    strlcpy(s_last_sha256, actual_hex, sizeof(s_last_sha256));
    s_ota_in_progress = false;

    ESP_LOGI(TAG, "Push OTA Succeeded! %d B -> %s (sha256=%s)",
             received, update_partition->label, actual_hex);

    char resp_json[256];
    snprintf(resp_json, sizeof(resp_json),
             "{\"ok\":true,\"partition\":\"%s\",\"bytes\":%d,\"sha256\":\"%s\",\"message\":\"OTA upgrade successful! Rebooting now...\"}",
             update_partition->label, received, actual_hex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_json);

    xTaskCreate(reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t ota_mgr_pull_from_url(const char *url, const char *expected_sha256, bool reboot_after)
{
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        snprintf(s_last_error, sizeof(s_last_error), "invalid url");
        return ESP_ERR_INVALID_ARG;
    }
    if (expected_sha256 && expected_sha256[0] != '\0' && !is_hex64(expected_sha256)) {
        snprintf(s_last_error, sizeof(s_last_error), "invalid sha256 format");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ota_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        snprintf(s_last_error, sizeof(s_last_error), "no passive OTA partition");
        return ESP_ERR_NOT_FOUND;
    }

    s_ota_in_progress = true;
    s_last_error[0] = '\0';
    emit_progress("downloading", 0, 0, 0, NULL);

    ESP_LOGI(TAG, "Pull OTA starting from %s -> partition %s", url, update_partition->label);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .buffer_size = 4096,
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(s_last_error, sizeof(s_last_error), "http client init failed");
        emit_progress("failed", 0, 0, 0, s_last_error);
        s_ota_in_progress = false;
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(s_last_error, sizeof(s_last_error), "http open: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_last_error);
        emit_progress("failed", 0, 0, 0, s_last_error);
        esp_http_client_cleanup(client);
        s_ota_in_progress = false;
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        snprintf(s_last_error, sizeof(s_last_error), "http status %d", status_code);
        ESP_LOGE(TAG, "Pull OTA %s", s_last_error);
        emit_progress("failed", 0, 0, (int)content_len, s_last_error);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_ota_in_progress = false;
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        snprintf(s_last_error, sizeof(s_last_error), "oom");
        emit_progress("failed", 0, 0, (int)content_len, s_last_error);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_ota_in_progress = false;
        return ESP_ERR_NO_MEM;
    }

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    esp_ota_handle_t ota_handle = 0;
    int total_read = 0;
    int last_reported_pct = -1;
    bool first_chunk = true;

    while (1) {
        int n = esp_http_client_read(client, buf, 4096);
        if (n < 0) {
            snprintf(s_last_error, sizeof(s_last_error), "http read err %d", n);
            ESP_LOGE(TAG, "%s", s_last_error);
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            break;
        }

        if (first_chunk) {
            first_chunk = false;
            if ((uint8_t)buf[0] != 0xE9) {
                snprintf(s_last_error, sizeof(s_last_error), "bad magic 0x%02X", (uint8_t)buf[0]);
                ESP_LOGE(TAG, "%s", s_last_error);
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
            if (err != ESP_OK) {
                snprintf(s_last_error, sizeof(s_last_error), "ota_begin: %s", esp_err_to_name(err));
                break;
            }
        }

        mbedtls_sha256_update(&sha_ctx, (const unsigned char *)buf, n);
        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) {
            snprintf(s_last_error, sizeof(s_last_error), "ota_write: %s", esp_err_to_name(err));
            break;
        }
        total_read += n;
        if (content_len > 0) {
            int pct = (int)(((int64_t)total_read * 100) / content_len);
            if (pct > 100) pct = 100;
            if (pct - last_reported_pct >= 10 || pct == 100) {
                last_reported_pct = pct;
                emit_progress("downloading", pct, total_read, (int)content_len, NULL);
            }
        }
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    uint8_t digest[32];
    char actual_hex[65];
    mbedtls_sha256_finish(&sha_ctx, digest);
    mbedtls_sha256_free(&sha_ctx);
    sha256_to_hex(digest, actual_hex);

    if (err != ESP_OK || total_read == 0 || (content_len > 0 && total_read != (int)content_len)) {
        if (s_last_error[0] == '\0') {
            snprintf(s_last_error, sizeof(s_last_error), "truncated download (%d/%lld)", total_read, (long long)content_len);
        }
        ESP_LOGE(TAG, "Pull OTA aborted: %s", s_last_error);
        emit_progress("failed", 0, total_read, (int)content_len, s_last_error);
        if (ota_handle) esp_ota_abort(ota_handle);
        s_ota_in_progress = false;
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    if (expected_sha256 && expected_sha256[0] != '\0' && strcasecmp(expected_sha256, actual_hex) != 0) {
        snprintf(s_last_error, sizeof(s_last_error), "SHA-256 mismatch");
        ESP_LOGE(TAG, "Pull OTA SHA-256 mismatch! expected=%s got=%s", expected_sha256, actual_hex);
        emit_progress("failed", 0, total_read, (int)content_len, s_last_error);
        esp_ota_abort(ota_handle);
        s_ota_in_progress = false;
        return ESP_ERR_INVALID_CRC;
    }

    emit_progress("verifying", 100, total_read, (int)content_len, NULL);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        snprintf(s_last_error, sizeof(s_last_error), "ota_end: %s", esp_err_to_name(err));
        emit_progress("failed", 0, total_read, (int)content_len, s_last_error);
        s_ota_in_progress = false;
        return err;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        snprintf(s_last_error, sizeof(s_last_error), "set_boot: %s", esp_err_to_name(err));
        emit_progress("failed", 0, total_read, (int)content_len, s_last_error);
        s_ota_in_progress = false;
        return err;
    }

    strlcpy(s_last_sha256, actual_hex, sizeof(s_last_sha256));
    s_ota_in_progress = false;

    ESP_LOGI(TAG, "Pull OTA Verified & Staged! %d B -> %s (sha256=%s)",
             total_read, update_partition->label, actual_hex);

    emit_progress(reboot_after ? "rebooting" : "done", 100, total_read, (int)content_len, NULL);
    if (reboot_after) {
        xTaskCreate(reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    }
    return ESP_OK;
}

static void ota_pull_bg_task(void *pv)
{
    ota_pull_job_t *job = (ota_pull_job_t *)pv;
    ota_mgr_pull_from_url(job->url, job->sha256, true);
    free(job);
    vTaskDelete(NULL);
}

esp_err_t ota_mgr_start_pull_async(const char *url, const char *expected_sha256)
{
    if (s_ota_in_progress) return ESP_ERR_INVALID_STATE;
    if (!url || strncmp(url, "http", 4) != 0) return ESP_ERR_INVALID_ARG;
    if (expected_sha256 && expected_sha256[0] != '\0' && !is_hex64(expected_sha256)) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_pull_job_t *job = calloc(1, sizeof(ota_pull_job_t));
    if (!job) return ESP_ERR_NO_MEM;
    strlcpy(job->url, url, sizeof(job->url));
    if (expected_sha256 && expected_sha256[0]) {
        strlcpy(job->sha256, expected_sha256, sizeof(job->sha256));
    }
    if (xTaskCreate(ota_pull_bg_task, "ota_pull", 8192, job, 5, NULL) != pdPASS) {
        free(job);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ota_pull_post_handler(httpd_req_t *req)
{
    if (s_ota_in_progress) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA already in progress");
        return ESP_FAIL;
    }

    int len = req->content_len;
    if (len <= 0 || len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body size");
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

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed JSON");
        return ESP_FAIL;
    }

    const cJSON *j_url = cJSON_GetObjectItem(root, "url");
    const cJSON *j_sha = cJSON_GetObjectItem(root, "sha256");
    if (!cJSON_IsString(j_url) || strncmp(j_url->valuestring, "http", 4) != 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing valid 'url'");
        return ESP_FAIL;
    }

    esp_err_t start_err = ota_mgr_start_pull_async(
        j_url->valuestring,
        cJSON_IsString(j_sha) ? j_sha->valuestring : NULL
    );
    cJSON_Delete(root);

    if (start_err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid 'sha256' or URL");
        return ESP_FAIL;
    }
    if (start_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start Pull OTA task");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Pull OTA started in background\"}");
    return ESP_OK;
}
