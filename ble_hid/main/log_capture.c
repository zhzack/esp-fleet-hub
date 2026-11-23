#include "log_capture.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static vprintf_like_t s_orig_vprintf = NULL;
static char *s_buffer = NULL;
static size_t s_capacity = 32768;
static size_t s_head = 0;
static uint32_t s_total_bytes = 0;
static volatile bool s_enabled = true;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static int log_vprintf_hook(const char *fmt, va_list args)
{
    int ret = 0;
    if (s_orig_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = s_orig_vprintf(fmt, args_copy);
        va_end(args_copy);
    } else {
        ret = vprintf(fmt, args);
    }

    if (!s_enabled || !s_buffer) {
        return ret;
    }

    char temp[256];
    int len = vsnprintf(temp, sizeof(temp), fmt, args);
    if (len > 0) {
        if (len >= (int)sizeof(temp)) len = sizeof(temp) - 1;
        taskENTER_CRITICAL(&s_mux);
        for (int i = 0; i < len; i++) {
            s_buffer[s_head] = temp[i];
            s_head = (s_head + 1) % s_capacity;
            s_total_bytes++;
        }
        taskEXIT_CRITICAL(&s_mux);
    }
    return ret;
}

esp_err_t log_capture_init(size_t capacity)
{
    s_capacity = capacity > 4096 ? capacity : 32768;
    s_buffer = malloc(s_capacity);
    if (!s_buffer) return ESP_ERR_NO_MEM;
    memset(s_buffer, 0, s_capacity);
    s_head = 0;
    s_total_bytes = 0;
    s_enabled = true;
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);
    return ESP_OK;
}

bool log_capture_is_enabled(void)
{
    return s_enabled;
}

esp_err_t log_capture_set_enabled(bool enable)
{
    s_enabled = enable;
    return ESP_OK;
}

void log_capture_clear(void)
{
    if (!s_buffer) return;
    taskENTER_CRITICAL(&s_mux);
    s_head = 0;
    s_total_bytes = 0;
    memset(s_buffer, 0, s_capacity);
    taskEXIT_CRITICAL(&s_mux);
}

size_t log_capture_read(char *out_buf, size_t max_len, uint32_t *seq)
{
    if (!s_buffer || !out_buf || max_len == 0 || !seq) return 0;
    taskENTER_CRITICAL(&s_mux);
    uint32_t total = s_total_bytes;
    uint32_t cur_seq = *seq;
    if (cur_seq > total) cur_seq = total;
    uint32_t unread = total - cur_seq;
    if (unread > s_capacity) {
        cur_seq = total - s_capacity;
        unread = s_capacity;
    }
    if (unread > max_len) unread = max_len;

    for (size_t i = 0; i < unread; i++) {
        size_t idx = (cur_seq + i) % s_capacity;
        out_buf[i] = s_buffer[idx];
    }
    *seq = cur_seq + unread;
    taskEXIT_CRITICAL(&s_mux);
    return unread;
}
