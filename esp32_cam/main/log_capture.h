#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the circular log buffer (e.g. 32KB). */
esp_err_t log_capture_init(size_t capacity);

/** Check whether log capture is currently enabled. */
bool log_capture_is_enabled(void);

/** Dynamically enable or disable log capture. */
esp_err_t log_capture_set_enabled(bool enable);

/** Clear all captured logs in the buffer. */
void log_capture_clear(void);

/**
 * Read up to max_len bytes of logs starting from *seq.
 * Updates *seq to the next sequence number.
 * Returns bytes copied into out_buf.
 */
size_t log_capture_read(char *out_buf, size_t max_len, uint32_t *seq);

#ifdef __cplusplus
}
#endif
