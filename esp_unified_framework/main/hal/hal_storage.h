#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hal_storage_init(void);
bool hal_storage_is_mounted(void);
esp_err_t hal_storage_get_usage(size_t *out_total, size_t *out_used);
const char *hal_storage_base_path(void);

#ifdef __cplusplus
}
#endif
