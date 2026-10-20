#pragma once
#include "esp_err.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t executor_start(uint16_t queue_size);
esp_err_t executor_enqueue(action_msg_t *am);
uint16_t  executor_queue_len(void);

/** Transport the executor uses for HID output. */
typedef enum {
    EXECUTOR_TRANSPORT_AUTO = 0,  /* prefer USB if mounted, else BLE */
    EXECUTOR_TRANSPORT_BLE,
    EXECUTOR_TRANSPORT_USB,
} executor_transport_t;

/* Build-time default transport selection.
 * 0 = AUTO, 1 = BLE, 2 = USB
 * Change this macro or override it from compiler flags when you want a
 * different startup transport. The current default is AUTO (prefer USB when
 * mounted, else BLE); an explicit transport.set is persisted to NVS by
 * cmd_router and restored on boot.
 */
#ifndef EXECUTOR_DEFAULT_TRANSPORT_MODE
#define EXECUTOR_DEFAULT_TRANSPORT_MODE 0
#endif

void                executor_set_transport(executor_transport_t t);
executor_transport_t executor_get_transport(void);

/** Currently-active transport after AUTO resolution (BLE or USB). */
executor_transport_t executor_active_transport(void);

/** Run an action payload (executor-step format) synchronously on the caller's task.
 *  Returns 0 on success, non-zero on error. err is filled with err_msg, errcode
 *  with the 4-char code (e.g. "E004") if non-NULL. */
int executor_run_payload_sync(const cJSON *payload, char *errcode, int codelen,
                              char *err, int errlen);

#ifdef __cplusplus
}
#endif
