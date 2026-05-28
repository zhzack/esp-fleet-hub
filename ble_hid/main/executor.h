#pragma once
#include "esp_err.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t executor_start(uint16_t queue_size);
esp_err_t executor_enqueue(action_msg_t *am);
uint16_t  executor_queue_len(void);

/** Run an action payload (executor-step format) synchronously on the caller's task.
 *  Returns 0 on success, non-zero on error. err is filled with err_msg, errcode
 *  with the 4-char code (e.g. "E004") if non-NULL. */
int executor_run_payload_sync(const cJSON *payload, char *errcode, int codelen,
                              char *err, int errlen);

/** Run an action payload (executor-step format) synchronously on the caller's task.
 *  Returns 0 on success, non-zero on error. err is filled with err_msg, errcode
 *  with the 4-char code (e.g. "E004") if non-NULL. */
int executor_run_payload_sync(const cJSON *payload, char *errcode, int codelen,
                              char *err, int errlen);

#ifdef __cplusplus
}
#endif
