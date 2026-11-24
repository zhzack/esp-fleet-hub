#include "log_capture.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

static char *s_log_buf = NULL;
static size_t s_buf_cap = 0;
static size_t s_write_idx = 0;
static size_t s_total_written = 0;
static SemaphoreHandle_t s_log_mux = NULL;
static vprintf_like_t s_prev_vprintf = NULL;

static int log_vprintf_hook(const char *fmt, va_list args)
{
    // First, pass through to UART / default handler
    va_list args_copy;
    va_copy(args_copy, args);
    if (s_prev_vprintf) {
        s_prev_vprintf(fmt, args_copy);
    } else {
        vprintf(fmt, args_copy);
    }
    va_end(args_copy);

    if (!s_log_buf || !s_log_mux) return 0;

    char line_buf[256];
    int len = vsnprintf(line_buf, sizeof(line_buf), fmt, args);
    if (len <= 0) return 0;
    if ((size_t)len >= sizeof(line_buf)) len = sizeof(line_buf) - 1;

    if (xSemaphoreTake(s_log_mux, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < len; i++) {
            s_log_buf[s_write_idx] = line_buf[i];
            s_write_idx = (s_write_idx + 1) % s_buf_cap;
            s_total_written++;
        }
        xSemaphoreGive(s_log_mux);
    }

    return len;
}

esp_err_t log_capture_init(size_t buffer_size)
{
    if (buffer_size == 0) buffer_size = 16384;

    s_log_mux = xSemaphoreCreateMutex();
    if (!s_log_mux) return ESP_ERR_NO_MEM;

    // Prefer allocating in PSRAM if available to save internal RAM
#if CONFIG_SPIRAM
    s_log_buf = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!s_log_buf) {
        s_log_buf = malloc(buffer_size);
    }
    if (!s_log_buf) {
        vSemaphoreDelete(s_log_mux);
        s_log_mux = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_buf_cap = buffer_size;
    s_write_idx = 0;
    s_total_written = 0;
    memset(s_log_buf, 0, s_buf_cap);

    s_prev_vprintf = esp_log_set_vprintf(log_vprintf_hook);
    return ESP_OK;
}

size_t log_capture_read(char *dst, size_t max_len)
{
    if (!dst || max_len < 2 || !s_log_buf || !s_log_mux) {
        if (dst && max_len > 0) dst[0] = '\0';
        return 0;
    }

    size_t copied = 0;
    if (xSemaphoreTake(s_log_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t available = (s_total_written < s_buf_cap) ? s_total_written : s_buf_cap;
        size_t to_copy = (available < max_len - 1) ? available : (max_len - 1);

        size_t start_idx = 0;
        if (s_total_written > s_buf_cap) {
            start_idx = (s_write_idx + s_buf_cap - to_copy) % s_buf_cap;
        } else {
            start_idx = (s_write_idx >= to_copy) ? (s_write_idx - to_copy) : 0;
        }

        for (size_t i = 0; i < to_copy; i++) {
            dst[i] = s_log_buf[(start_idx + i) % s_buf_cap];
        }
        dst[to_copy] = '\0';
        copied = to_copy;

        xSemaphoreGive(s_log_mux);
    }

    return copied;
}

void log_capture_clear(void)
{
    if (s_log_mux && xSemaphoreTake(s_log_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_write_idx = 0;
        s_total_written = 0;
        if (s_log_buf) memset(s_log_buf, 0, s_buf_cap);
        xSemaphoreGive(s_log_mux);
    }
}
