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

/** 服务端 auth_challenge 处理：计算 HMAC 并携带 proof 重新注册。 */
void ws_client_handle_auth_challenge(const char *nonce);

#ifdef __cplusplus
}
#endif
