#ifndef RECOVERY_USER_LIBSTARTUP_OPTIONS_UI_FONT_SANS_H
#define RECOVERY_USER_LIBSTARTUP_OPTIONS_UI_FONT_SANS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct startup_options_ui_font_glyph {
	uint32_t offset;
	uint8_t width;
	uint8_t height;
	int8_t x_offset;
	int8_t y_offset;
	uint8_t advance;
} startup_options_ui_font_glyph_t;

typedef struct startup_options_ui_font_face {
	const uint8_t *pixels;
	const startup_options_ui_font_glyph_t *glyphs;
	uint8_t line_height;
} startup_options_ui_font_face_t;

const startup_options_ui_font_face_t *startup_options_ui_font_face(uint32_t scale);
const startup_options_ui_font_face_t *startup_options_ui_font_face_for_weight(uint32_t scale, uint32_t weight);
bool startup_options_ui_font_has_weights(void);
const startup_options_ui_font_glyph_t *startup_options_ui_font_lookup(const startup_options_ui_font_face_t *face, char character);
const uint8_t *startup_options_ui_font_pixels(const startup_options_ui_font_face_t *face);
uint32_t startup_options_ui_font_line_height(const startup_options_ui_font_face_t *face);

#endif
