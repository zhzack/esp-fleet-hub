#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ws_gateway_init(void);
bool ws_gateway_is_online(void);
void ws_gateway_apply_config(void);
void ws_gateway_get_active_url(char *dst, size_t max_len);

#ifdef __cplusplus
}
#endif
