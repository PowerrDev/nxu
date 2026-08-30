#ifndef NXU_RECOVERY_FONT_TTF_H
#define NXU_RECOVERY_FONT_TTF_H

#include <Recovery/drawing.h>

#include <stdbool.h>
#include <stdint.h>

bool startup_options_ui_ttf_mono_available(void);
uint32_t startup_options_ui_ttf_mono_text_width(const char *text, uint32_t scale);
uint32_t startup_options_ui_ttf_mono_line_height(uint32_t scale);
uint32_t startup_options_ui_ttf_sans_line_height(uint32_t scale);
bool startup_options_ui_ttf_draw_mono_text(startup_options_ui_canvas_t *canvas, int32_t x, int32_t y, const char *text, uint32_t color, uint32_t scale);

#endif
