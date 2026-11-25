#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t fleet_agent_init(void);
bool fleet_agent_is_registered(void);
void fleet_agent_get_hub_url(char *dst, size_t max_len);

#ifdef __cplusplus
}
#endif
