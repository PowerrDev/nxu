#ifndef RECOVERY_USER_LIBSTARTUP_OPTIONS_UI_DRAWING_INTERNAL_H
#define RECOVERY_USER_LIBSTARTUP_OPTIONS_UI_DRAWING_INTERNAL_H

#include <Recovery/drawing.h>

#include <stdint.h>

/* Blend one glyph-coverage sample into an application-owned ARGB canvas. */
void startup_options_ui_canvas_blend_coverage(
	startup_options_ui_canvas_t *canvas,
	int32_t x,
	int32_t y,
	uint32_t color,
	uint32_t coverage
);

#endif
