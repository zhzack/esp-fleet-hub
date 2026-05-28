#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Launch heartbeat task; sends `heartbeat` envelope every period_sec. */
esp_err_t heartbeat_start(int period_sec);

#ifdef __cplusplus
}
#endif
