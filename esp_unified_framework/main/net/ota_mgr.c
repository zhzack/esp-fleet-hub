#include "ota_mgr.h"

#include <string.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota_mgr";

static void reboot_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Rebooting system in 1.5s after OTA flash...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

esp_err_t ota_mgr_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition: %s (offset 0x%08lx, size 0x%08lx)",
             running ? running->label : "unknown",
             running ? running->address : 0,
             running ? running->size : 0);
    return ESP_OK;
}

size_t ota_mgr_status_to_json(char *dst, size_t max_len)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t app_desc;
    esp_ota_get_partition_description(running, &app_desc);

    return snprintf(dst, max_len,
        "{"
          "\"ok\":true,"
          "\"running_partition\":\"%s\","
          "\"next_partition\":\"%s\","
          "\"version\":\"%s\","
          "\"project_name\":\"%s\","
          "\"idf_ver\":\"%s\","
          "\"compile_time\":\"%s %s\""
        "}",
        running ? running->label : "unknown",
        next ? next->label : "none",
        app_desc.version,
        app_desc.project_name,
        app_desc.idf_ver,
        app_desc.date, app_desc.time
    );
}

esp_err_t ota_upload_post_handler(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    if (!update_partition) {
        ESP_LOGE(TAG, "Passive OTA partition not found!");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA partition not found");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Writing OTA to partition: %s (address 0x%08lx, size 0x%08lx)",
             update_partition->label, update_partition->address, update_partition->size);

    int content_len = req->content_len;
    if (content_len <= 0) {
        ESP_LOGE(TAG, "Invalid content length: %d", content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Length required");
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int received = 0;
    int remaining = content_len;
    bool is_first_chunk = true;
    int timeout_retries = 0;
    esp_err_t err = ESP_OK;

    while (remaining > 0) {
        int to_read = remaining < 4096 ? remaining : 4096;
        int read_bytes = httpd_req_recv(req, buf, to_read);

        if (read_bytes <= 0) {
            if (read_bytes == HTTPD_SOCK_ERR_TIMEOUT) {
                timeout_retries++;
                if (timeout_retries < 5) {
                    continue;
                }
            }
            ESP_LOGE(TAG, "Socket receive error or timeout exceeded: %d", read_bytes);
            free(buf);
            if (ota_handle) esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Stream recv failed");
            return ESP_FAIL;
        }
        timeout_retries = 0;

        if (is_first_chunk) {
            is_first_chunk = false;
            // Validate ESP image magic byte (0xE9)
            if ((uint8_t)buf[0] != 0xE9) {
                ESP_LOGE(TAG, "Invalid firmware magic byte: 0x%02X (expected 0xE9)", (uint8_t)buf[0]);
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware image header");
                return ESP_FAIL;
            }

            err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
                free(buf);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
                return ESP_FAIL;
            }
        }

        err = esp_ota_write(ota_handle, buf, read_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write failed");
            return ESP_FAIL;
        }

        received += read_bytes;
        remaining -= read_bytes;
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_end validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_set_boot_partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA Upload Succeeded! %d bytes written to %s", received, update_partition->label);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"OTA upgrade successful! Rebooting now...\"}");

    // Spawn async reboot task
    xTaskCreate(reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}
