#ifndef CMD_ROUTER_H
#define CMD_ROUTER_H

#include <stdbool.h>
#include "cJSON.h"

typedef int (*cmd_handler_fn)(const cJSON *payload, cJSON *result, char *err, int errlen);

void cmd_router_init(void);
int  cmd_router_register(const char *type, cmd_handler_fn fn);

/* Dispatch a command.
 *   type:    command type string (e.g. "get_status", "action.press").
 *   payload: input payload object (may be NULL).
 *   result:  pre-allocated cJSON object the handler appends fields into
 *            (caller owns it; pass a freshly created object).
 *   Returns 0 on success, non-zero code otherwise (sets err).
 *   Returns 404 ("unknown type") when no handler is registered. */
int  cmd_router_dispatch(const char *type, const cJSON *payload, cJSON *result, char *err, int errlen);

void cmd_router_set_device_info(const char *device_id, const char *fw);

#endif
