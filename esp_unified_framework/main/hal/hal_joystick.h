#ifndef JOYSTICK_H
#define JOYSTICK_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    JOYSTICK_NONE = 0,
    JOYSTICK_UP,
    JOYSTICK_DOWN,
    JOYSTICK_LEFT,
    JOYSTICK_RIGHT,
    JOYSTICK_CENTER
} joystick_key_t;

typedef void (*joystick_callback_t)(joystick_key_t key);

esp_err_t joystick_init(joystick_callback_t cb);

#endif // JOYSTICK_H
