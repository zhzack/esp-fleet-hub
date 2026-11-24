#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize in-memory ring buffer log capture hooking into esp_log_set_vprintf
 * @param buffer_size Size of circular buffer in bytes (e.g. 16384)
 */
esp_err_t log_capture_init(size_t buffer_size);

/**
 * @brief Read recent logs into dst buffer (null-terminated)
 * @param dst Target buffer
 * @param max_len Maximum bytes to copy
 * @return Bytes copied
 */
size_t log_capture_read(char *dst, size_t max_len);

/**
 * @brief Clear internal log buffer
 */
void log_capture_clear(void);

#ifdef __cplusplus
}
#endif
