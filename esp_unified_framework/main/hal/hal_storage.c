#include "hal_storage.h"
#include "sdkconfig.h"
#include "esp_log.h"

#if CONFIG_FEATURE_STORAGE
#include "esp_spiffs.h"

static const char *TAG = "hal_storage";
static bool s_mounted = false;

esp_err_t hal_storage_init(void)
{
    if (s_mounted) return ESP_OK;

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format SPIFFS partition 'storage'");
        } else if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Partition 'storage' not found in current partition table (skipping mount)");
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(err));
        }
        return err;
    }

    s_mounted = true;
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS 'storage' mounted @ /storage: %u / %u bytes used",
                 (unsigned)used, (unsigned)total);
    }
    return ESP_OK;
}

bool hal_storage_is_mounted(void)
{
    return s_mounted;
}

esp_err_t hal_storage_get_usage(size_t *out_total, size_t *out_used)
{
    if (!s_mounted) {
        if (out_total) *out_total = 0;
        if (out_used) *out_used = 0;
        return ESP_ERR_INVALID_STATE;
    }
    return esp_spiffs_info("storage", out_total, out_used);
}

const char *hal_storage_base_path(void)
{
    return "/storage";
}

#else

esp_err_t hal_storage_init(void) { return ESP_OK; }
bool hal_storage_is_mounted(void) { return false; }
esp_err_t hal_storage_get_usage(size_t *out_total, size_t *out_used)
{
    if (out_total) *out_total = 0;
    if (out_used) *out_used = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
const char *hal_storage_base_path(void) { return "/storage"; }

#endif
