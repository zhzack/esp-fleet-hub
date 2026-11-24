#ifndef FONT8X16_H
#define FONT8X16_H

#include <stdint.h>

// Standard 8x16 ASCII Font (Characters 32 to 126)
// Each character is 16 bytes; each byte represents 8 horizontal pixels (MSB on the left)
extern const uint8_t font8x16[95][16];

#endif // FONT8X16_H
