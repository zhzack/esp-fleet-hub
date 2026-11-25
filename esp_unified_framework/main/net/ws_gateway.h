#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ws_gateway_init(void);
bool ws_gateway_is_online(void);

#ifdef __cplusplus
}
#endif
