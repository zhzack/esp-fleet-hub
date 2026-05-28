#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Connect and start RX/TX loops. */
esp_err_t ws_client_start(const char *host, uint16_t port,
                          const char *device_id, const char *token);

/** Enqueue envelope (takes ownership of json — cJSON_Delete called after send). */
esp_err_t ws_client_send(cJSON *envelope);

/** True when register-ack received. */
bool ws_client_online(void);

#ifdef __cplusplus
}
#endif
