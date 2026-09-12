#ifndef NXU_KERN_CONSOLE_FONT8X16_H
#define NXU_KERN_CONSOLE_FONT8X16_H

#include <stdint.h>

#define FONT8X16_WIDTH 8U
#define FONT8X16_HEIGHT 16U

/* One 16-byte glyph bitmap per CP437-ish code point, row-major, LSB-first column. */
extern const uint8_t g_font8x16[256U * FONT8X16_HEIGHT];

#endif
