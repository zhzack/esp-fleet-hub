#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Translate a backend key name (e.g. "KEY_A", "KEY_ENTER", "KEY_CTRL_C") to HID usage + modifiers. */
bool keymap_lookup(const char *name, uint8_t *out_code, uint8_t *out_mods);

/** Translate a single ASCII character to HID usage + modifiers. */
bool keymap_ascii(char c, uint8_t *out_code, uint8_t *out_mods);

/** Translate a multimedia/consumer key name (e.g. "KEY_MUTE", "KEY_VOL_UP") to 16-bit Consumer usage code. */
bool keymap_consumer_lookup(const char *name, uint16_t *out_code);

#ifdef __cplusplus
}
#endif
