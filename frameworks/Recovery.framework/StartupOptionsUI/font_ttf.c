#include "font_ttf.h"

bool startup_options_ui_ttf_mono_available(void)
{
	return false;
}

uint32_t startup_options_ui_ttf_mono_text_width(const char *text, uint32_t scale)
{
	(void)text;
	(void)scale;
	return 0U;
}

uint32_t startup_options_ui_ttf_mono_line_height(uint32_t scale)
{
	(void)scale;
	return 0U;
}

uint32_t startup_options_ui_ttf_sans_line_height(uint32_t scale)
{
	(void)scale;
	return 0U;
}

bool startup_options_ui_ttf_draw_mono_text(startup_options_ui_canvas_t *canvas, int32_t x, int32_t y, const char *text, uint32_t color, uint32_t scale)
{
	(void)canvas;
	(void)x;
	(void)y;
	(void)text;
	(void)color;
	(void)scale;
	return false;
}
