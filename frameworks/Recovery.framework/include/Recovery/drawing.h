#ifndef RECOVERY_USER_LIBSTARTUP_OPTIONS_UI_DRAWING_H
#define RECOVERY_USER_LIBSTARTUP_OPTIONS_UI_DRAWING_H

#include <stdbool.h>
#include <stdint.h>

#define STARTUP_OPTIONS_UI_ARGB(a, r, g, b) \
	((((uint32_t)(a)) << 24U) | (((uint32_t)(r)) << 16U) | (((uint32_t)(g)) << 8U) | (uint32_t)(b))

/* Software drawing primitives for application-owned ARGB surfaces. */
typedef struct {
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
} startup_options_ui_canvas_t;

/*
 * Recovery text keeps the familiar CSS weight scale in its API. The bootstrap
 * raster family contains one real weight; semibold and heavier requests are
 * rendered with a one-pixel overdraw rather than depending on the main UI font
 * stack.
 */
typedef enum {
	STARTUP_OPTIONS_UI_FONT_WEIGHT_THIN = 100U,
	STARTUP_OPTIONS_UI_FONT_WEIGHT_EXTRALIGHT = 200U,
	STARTUP_OPTIONS_UI_FONT_WEIGHT_LIGHT = 300U,
	STARTUP_OPTIONS_UI_FONT_WEIGHT_REGULAR = 400U,
	STARTUP_OPTIONS_UI_FONT_WEIGHT_MEDIUM = 500U,
	STARTUP_OPTIONS_UI_FONT_WEIGHT_SEMIBOLD = 600U,
	STARTUP_OPTIONS_UI_FONT_WEIGHT_BOLD = 700U,
	STARTUP_OPTIONS_UI_FONT_WEIGHT_EXTRABOLD = 800U,
	STARTUP_OPTIONS_UI_FONT_WEIGHT_BLACK = 900U
} startup_options_ui_font_weight_t;

/*
 *	Routine:	startup_options_ui_font_preload_sans, startup_options_ui_font_preload_mono
 *	Purpose:
 *		Confirm that the built-in recovery raster faces are resident.
 *	Returns:
 *		true; the recovery faces are linked into triageOS.
 */
bool startup_options_ui_font_preload_sans(void);
bool startup_options_ui_font_preload_mono(void);

/*
 *	Routine:	startup_options_ui_font_sans_has_weights, startup_options_ui_font_sans_has_italic
 *	Purpose:
 *		Report native style capabilities of the built-in recovery face.
 *	Discussion:
 *		Recovery deliberately has no dependency on the normal sevOS font
 *		stack. Styled drawing degrades to the bootstrap raster face.
 */
bool startup_options_ui_font_sans_has_weights(void);
bool startup_options_ui_font_sans_has_italic(void);

bool startup_options_ui_canvas_init(startup_options_ui_canvas_t *canvas, uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride);
void startup_options_ui_canvas_clear(startup_options_ui_canvas_t *canvas, uint32_t color);
/*
 *	Routine:	startup_options_ui_canvas_replace_rect
 *	Purpose:
 *		Replace pixels verbatim, including their alpha.
 *	Discussion:
 *		Translucent materials must be replaced rather than blended so the
 *		recovery surface retains its source alpha instead of flattening it
 *		onto pixels from an earlier frame.
 */
void startup_options_ui_canvas_replace_rect(startup_options_ui_canvas_t *canvas, int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t color);
/*
 *	Routine:	startup_options_ui_canvas_replace_squircle
 *	Purpose:
 *		Replace a rounded material without flattening its alpha into the
 *		backing pixels.
 */
void startup_options_ui_canvas_replace_squircle(
	startup_options_ui_canvas_t *canvas,
	int32_t x,
	int32_t y,
	uint32_t width,
	uint32_t height,
	uint32_t radius,
	uint32_t color
);
void startup_options_ui_canvas_fill_rect(startup_options_ui_canvas_t *canvas, int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t color);
void startup_options_ui_canvas_fill_vertical_gradient(startup_options_ui_canvas_t *canvas, uint32_t top, uint32_t bottom);

/*
 *	Routine:	startup_options_ui_canvas_fill_squircle
 *	Purpose:
 *		Fill a fourth order superellipse using integer arithmetic only.
 *	Discussion:
 *		A superellipse rather than a circular arc is what makes the
 *		corner curvature continuous, which is why system controls read
 *		as rounded rather than as a rectangle with quarter circles
 *		pasted on.
 */
void startup_options_ui_canvas_fill_squircle(
	startup_options_ui_canvas_t *canvas,
	int32_t x,
	int32_t y,
	uint32_t width,
	uint32_t height,
	uint32_t radius,
	uint32_t color
);

/*
 *	Routine:	startup_options_ui_canvas_fill_squircle_gradient
 *	Purpose:
 *		Fill a rounded material whose color ramps from top to bottom.
 *	Discussion:
 *		System icons and controls are lit from above.  A flat fill reads
 *		as a colored rectangle; the vertical ramp is what makes the same
 *		shape read as a physical surface.
 */
void startup_options_ui_canvas_fill_squircle_gradient(
	startup_options_ui_canvas_t *canvas,
	int32_t x,
	int32_t y,
	uint32_t width,
	uint32_t height,
	uint32_t radius,
	uint32_t top_color,
	uint32_t bottom_color
);

void startup_options_ui_canvas_stroke_squircle(
	startup_options_ui_canvas_t *canvas,
	int32_t x,
	int32_t y,
	uint32_t width,
	uint32_t height,
	uint32_t radius,
	uint32_t thickness,
	uint32_t border_color,
	uint32_t fill_color
);

void startup_options_ui_canvas_shadow_squircle(
	startup_options_ui_canvas_t *canvas,
	int32_t x,
	int32_t y,
	uint32_t width,
	uint32_t height,
	uint32_t radius,
	uint32_t spread,
	int32_t offset_y,
	uint32_t alpha
);

void startup_options_ui_canvas_fill_circle(
	startup_options_ui_canvas_t *canvas,
	int32_t center_x,
	int32_t center_y,
	uint32_t radius,
	uint32_t color
);

bool startup_options_ui_point_in_squircle(
	int32_t point_x,
	int32_t point_y,
	int32_t x,
	int32_t y,
	uint32_t width,
	uint32_t height,
	uint32_t radius
);

uint32_t startup_options_ui_text_width(const char *text, uint32_t scale);
/*
 * Height of one line of sans text: ascender plus descender.  Text is drawn from
 * the top of that line box, not from the cap, so centring a label inside a
 * control means subtracting this rather than the visible height of the glyphs.
 */
uint32_t startup_options_ui_line_height(uint32_t scale);
void startup_options_ui_draw_text(
	startup_options_ui_canvas_t *canvas,
	int32_t x,
	int32_t y,
	const char *text,
	uint32_t color,
	uint32_t scale
);

/*
 *	Routine:	startup_options_ui_styled_text_width, startup_options_ui_draw_styled_text
 *	Purpose:
 *		Measure and draw text at any weight, upright or italic.
 *	Conditions:
 *		`weight` is an startup_options_ui_font_weight_t, or any value between 100 and
 *		900 which is snapped to the nearest supported master.  Italic
 *		requests fall back to upright when no italic file is installed.
 */
uint32_t startup_options_ui_styled_text_width(const char *text, uint32_t scale, uint32_t weight, bool italic);
void startup_options_ui_draw_styled_text(
	startup_options_ui_canvas_t *canvas,
	int32_t x,
	int32_t y,
	const char *text,
	uint32_t color,
	uint32_t scale,
	uint32_t weight,
	bool italic
);

/*
 *	Routine:	startup_options_ui_semibold_text_width, startup_options_ui_draw_semibold_text
 *	Purpose:
 *		Measure and draw semibold text.
 *	Discussion:
 *		A convenience spelling of the styled entry points at
 *		STARTUP_OPTIONS_UI_FONT_WEIGHT_SEMIBOLD, kept because system chrome asks for
 *		this one weight constantly.
 */
uint32_t startup_options_ui_semibold_text_width(const char *text, uint32_t scale);
void startup_options_ui_draw_semibold_text(
	startup_options_ui_canvas_t *canvas,
	int32_t x,
	int32_t y,
	const char *text,
	uint32_t color,
	uint32_t scale
);


uint32_t startup_options_ui_mono_text_width(const char *text, uint32_t scale);
uint32_t startup_options_ui_mono_line_height(uint32_t scale);
void startup_options_ui_draw_mono_text(
	startup_options_ui_canvas_t *canvas,
	int32_t x,
	int32_t y,
	const char *text,
	uint32_t color,
	uint32_t scale
);

#endif
