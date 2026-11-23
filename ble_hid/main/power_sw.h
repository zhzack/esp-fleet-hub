#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEFAULT_POWER_SW_GPIO 4

/**
 * Initialize the hardware power switch jumper GPIO.
 * Pin is kept in High-Z / Input mode by default to ensure safe non-interference.
 */
esp_err_t power_sw_init(uint8_t gpio_num);

/**
 * Simulate pressing the chassis power button for duration_ms (default 500ms).
 * Pin pulls LOW (or active level) then restores to High-Z.
 */
esp_err_t power_sw_pulse(int duration_ms);

/** Get current configured GPIO pin. */
uint8_t power_sw_get_pin(void);

#ifdef __cplusplus
}
#endif
