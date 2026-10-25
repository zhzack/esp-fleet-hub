#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_STATUS_IDLE = 0,
    OTA_STATUS_STARTING,
    OTA_STATUS_DOWNLOADING,
    OTA_STATUS_SUCCESS,
    OTA_STATUS_FAILED
} ota_status_t;

/* OTA 健康自检状态（仅当本次启动的分区处于 PENDING_VERIFY 时有意义） */
typedef enum {
    OTA_HEALTH_NOT_NEEDED = 0,   /* 本次启动非 OTA 首启（线刷/已确认），无需自检 */
    OTA_HEALTH_MONITORING,       /* 自检进行中：等待健康判据或超时 */
    OTA_HEALTH_PASSED,           /* 健康，已 mark valid（本分区确认为正式分区） */
    OTA_HEALTH_ROLLBACK_TIMEOUT  /* 超时不健康，已触发回滚重启（即将退出） */
} ota_health_t;

typedef struct {
    ota_status_t status;
    int          progress;       /* 0 - 100 */
    int          bytes_read;
    int          total_bytes;
    char         err_msg[96];
    char         running_partition[16];
    char         target_partition[16];
    char         version[32];
    ota_health_t health;         /* 本次启动的健康自检状态 */
} ota_info_t;

void ota_mgr_init(void);

/**
 * Start OTA update task.
 * url: firmware download URL
 * version: target firmware version (e.g. "v1.2.0")
 * sha256: optional expected sha256 hex string (NULL if not checked)
 */
esp_err_t ota_mgr_start(const char *url, const char *version, const char *sha256);

/**
 * Get current OTA state.
 */
void ota_mgr_get_info(ota_info_t *out);

/**
 * Build status cJSON for API response.
 */
void ota_mgr_build_json(cJSON *dst);

#ifdef __cplusplus
}
#endif
