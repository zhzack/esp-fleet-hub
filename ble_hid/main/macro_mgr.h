#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize macro manager. */
esp_err_t macro_mgr_init(void);

/** Get all macros as a cJSON Array (caller must cJSON_Delete). */
cJSON *macro_mgr_get_all(void);

/** Save or update a macro object (must contain "id", "name", and action payload). */
esp_err_t macro_mgr_save(const cJSON *macro_obj);

/** Delete a macro by its ID. */
esp_err_t macro_mgr_delete(const char *id);

/** Execute a macro by its ID synchronously. */
int macro_mgr_exec(const char *id, char *err, int errlen);

#ifdef __cplusplus
}
#endif
