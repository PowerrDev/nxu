#include "ui.h"

#include <Recovery/StartupOptionsUI.h>
#include <Recovery/RecoveryServices.h>

#include <stdbool.h>
#include <stdint.h>


#define TRIAGE_SYSTEM_DEVICE "disk0p2"
#define TRIAGE_SYSTEM_PARTITION_NAME "sevOS System"
#define TRIAGE_BG_TOP STARTUP_OPTIONS_UI_ARGB(255U, 15U, 16U, 19U)
#define TRIAGE_BG_BOTTOM STARTUP_OPTIONS_UI_ARGB(255U, 9U, 10U, 12U)
#define TRIAGE_BAR STARTUP_OPTIONS_UI_ARGB(255U, 20U, 21U, 25U)
#define TRIAGE_BAR_HEIGHT 38U
#define TRIAGE_ROW_TEXT_GAP 7U
#define TRIAGE_TERMINAL_HEADER_HEIGHT 64U
#define TRIAGE_TERMINAL_HEADER_GAP 8U
#define TRIAGE_PANEL STARTUP_OPTIONS_UI_ARGB(255U, 25U, 26U, 30U)
#define TRIAGE_BORDER STARTUP_OPTIONS_UI_ARGB(255U, 54U, 56U, 63U)
#define TRIAGE_ROW STARTUP_OPTIONS_UI_ARGB(255U, 31U, 33U, 38U)
#define TRIAGE_ROW_HOVER STARTUP_OPTIONS_UI_ARGB(255U, 38U, 40U, 46U)
#define TRIAGE_ROW_SELECTED STARTUP_OPTIONS_UI_ARGB(255U, 29U, 43U, 58U)
#define TRIAGE_ROW_PRESSED STARTUP_OPTIONS_UI_ARGB(255U, 25U, 36U, 49U)
#define TRIAGE_TEXT STARTUP_OPTIONS_UI_ARGB(255U, 241U, 242U, 244U)
#define TRIAGE_MUTED STARTUP_OPTIONS_UI_ARGB(255U, 151U, 154U, 162U)
#define TRIAGE_ACCENT STARTUP_OPTIONS_UI_ARGB(255U, 24U, 126U, 238U)
#define TRIAGE_ACCENT_HOVER STARTUP_OPTIONS_UI_ARGB(255U, 44U, 141U, 247U)
#define TRIAGE_ACCENT_PRESSED STARTUP_OPTIONS_UI_ARGB(255U, 18U, 104U, 201U)
#define TRIAGE_CONTINUE STARTUP_OPTIONS_UI_ARGB(255U, 96U, 99U, 106U)
#define TRIAGE_CONTINUE_HOVER STARTUP_OPTIONS_UI_ARGB(255U, 113U, 116U, 123U)
#define TRIAGE_CONTINUE_PRESSED STARTUP_OPTIONS_UI_ARGB(255U, 82U, 85U, 92U)
#define TRIAGE_CONTINUE_TEXT STARTUP_OPTIONS_UI_ARGB(255U, 241U, 242U, 244U)
#define TRIAGE_MODAL STARTUP_OPTIONS_UI_ARGB(255U, 29U, 30U, 35U)
#define TRIAGE_SHEET STARTUP_OPTIONS_UI_ARGB(255U, 44U, 45U, 50U)
#define TRIAGE_SHEET_BORDER STARTUP_OPTIONS_UI_ARGB(255U, 68U, 70U, 78U)
#define TRIAGE_SHEET_FIELD STARTUP_OPTIONS_UI_ARGB(255U, 25U, 26U, 30U)
#define TRIAGE_SHEET_FIELD_BORDER STARTUP_OPTIONS_UI_ARGB(255U, 79U, 81U, 89U)
#define TRIAGE_SHEET_SEGMENT STARTUP_OPTIONS_UI_ARGB(255U, 58U, 60U, 67U)
#define TRIAGE_SHEET_SEGMENT_HOVER STARTUP_OPTIONS_UI_ARGB(255U, 68U, 70U, 78U)
#define TRIAGE_SHEET_SEGMENT_ON STARTUP_OPTIONS_UI_ARGB(255U, 104U, 107U, 116U)
#define TRIAGE_SHEET_TRACK STARTUP_OPTIONS_UI_ARGB(255U, 63U, 65U, 72U)
#define TRIAGE_SHEET_KNOB STARTUP_OPTIONS_UI_ARGB(255U, 246U, 246U, 248U)
#define TRIAGE_SHEET_ACCENT_OFF STARTUP_OPTIONS_UI_ARGB(255U, 34U, 62U, 94U)
#define TRIAGE_MODAL_DIM STARTUP_OPTIONS_UI_ARGB(170U, 0U, 0U, 0U)
#define TRIAGE_TERMINAL STARTUP_OPTIONS_UI_ARGB(255U, 10U, 10U, 11U)
#define TRIAGE_TERMINAL_TEXT STARTUP_OPTIONS_UI_ARGB(255U, 236U, 236U, 236U)
#define TRIAGE_TERMINAL_MUTED STARTUP_OPTIONS_UI_ARGB(255U, 151U, 151U, 151U)
#define TRIAGE_TERMINAL_BORDER STARTUP_OPTIONS_UI_ARGB(255U, 54U, 54U, 56U)
#define TRIAGE_DISK_USED STARTUP_OPTIONS_UI_ARGB(255U, 46U, 126U, 231U)
#define TRIAGE_DISK_FREE STARTUP_OPTIONS_UI_ARGB(255U, 70U, 73U, 80U)
#define TRIAGE_DISK_ALT1 STARTUP_OPTIONS_UI_ARGB(255U, 126U, 87U, 194U)
#define TRIAGE_DISK_ALT2 STARTUP_OPTIONS_UI_ARGB(255U, 46U, 160U, 100U)
#define TRIAGE_DISK_ALT3 STARTUP_OPTIONS_UI_ARGB(255U, 218U, 139U, 60U)
#define TRIAGE_DISK_EXISTING STARTUP_OPTIONS_UI_ARGB(255U, 108U, 116U, 132U)

#define TRIAGE_KEY_ESCAPE 1U
#define TRIAGE_KEY_BACKSPACE 14U
#define TRIAGE_KEY_TAB 15U
#define TRIAGE_KEY_ENTER 28U
#define TRIAGE_KEY_UP 103U
#define TRIAGE_KEY_DOWN 108U
#define TRIAGE_KEY_R 19U
#define TRIAGE_KEY_ZERO 11U
#define TRIAGE_KEY_MINUS 12U
#define TRIAGE_KEY_EQUAL 13U

static uint32_t
triage_min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static uint32_t
triage_length(const char *text)
{
	uint32_t length = 0U;
	if (text != 0) {
		while (text[length] != '\0') {
			length++;
		}
	}

	return length;
}

static void
triage_copy(char *destination, uint32_t capacity, const char *source)
{
	if (destination == 0 || capacity == 0U) {
		return;
	}

	uint32_t index = 0U;
	if (source != 0) {
		while (source[index] != '\0' && index + 1U < capacity) {
			destination[index] = source[index];
			index++;
		}
	}

	destination[index] = '\0';
}

static bool
triage_equal(const char *left, const char *right)
{
	if (left == 0 || right == 0) return false;

	uint32_t index = 0U;
	while (left[index] != '\0' && right[index] != '\0') {
		if (left[index] != right[index]) return false;
		index++;
	}

	return left[index] == right[index];
}

static bool
triage_device_index(const char *name, uint32_t *index_out)
{
	if (name == 0 || index_out == 0) return false;

	for (uint32_t index = 0U; ; index++) {
		recovery_block_info_t info;
		if (recovery_block_info(index, &info) < 0) break;
		if (!triage_equal(info.name, name)) continue;

		*index_out = index;
		return true;
	}

	return false;
}


static void
triage_u64(char *buffer, uint32_t capacity, uint64_t value)
{
	if (buffer == 0 || capacity == 0U) {
		return;
	}

	char reversed[24];
	uint32_t count = 0U;
	do {
		reversed[count++] = (char)('0' + (value % 10ULL));
		value /= 10ULL;
	} while (value != 0ULL && count < sizeof(reversed));
	uint32_t out = 0U;
	while (count != 0U && out + 1U < capacity) {
		buffer[out++] = reversed[--count];
	}

	buffer[out] = '\0';
}

static void
triage_append(char *buffer, uint32_t capacity, const char *text)
{
	if (buffer == 0 || capacity == 0U || text == 0) {
		return;
	}

	uint32_t out = triage_length(buffer);
	for (uint32_t i = 0U; text[i] != '\0' && out + 1U < capacity; i++) {
		buffer[out++] = text[i];
	}

	buffer[out] = '\0';
}

static void
triage_format_bytes(char *buffer, uint32_t capacity, uint64_t bytes)
{
	uint64_t value = bytes;
	const char *unit = " B";
	if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
		value = bytes / (1024ULL * 1024ULL * 1024ULL);
		unit = " GiB";
	} else if (bytes >= 1024ULL * 1024ULL) {
		value = bytes / (1024ULL * 1024ULL);
		unit = " MiB";
	} else if (bytes >= 1024ULL) {
		value = bytes / 1024ULL;
		unit = " KiB";
	}

	triage_u64(buffer, capacity, value);
	triage_append(buffer, capacity, unit);
}

static const char *
triage_layout_name(uint32_t scheme)
{
	if (scheme == RECOVERY_BLOCK_LAYOUT_GPT) {
		return "GPT";
	}

	if (scheme == RECOVERY_BLOCK_LAYOUT_MBR) {
		return "MBR";
	}

	return "Raw";
}

static void
triage_workspace(const triage_ui_t *ui, int32_t *x, int32_t *y, uint32_t *width, uint32_t *height)
{
	uint32_t horizontal = ui->canvas.width > 900U ? 64U : 24U;
	uint32_t top = 58U;
	uint32_t bottom = 30U;
	*x = (int32_t)horizontal;
	*y = (int32_t)top;
	*width = ui->canvas.width > horizontal * 2U ? ui->canvas.width - horizontal * 2U : ui->canvas.width;
	*height = ui->canvas.height > top + bottom ? ui->canvas.height - top - bottom : ui->canvas.height;
}

static uint32_t
triage_background_color(const triage_ui_t *ui, uint32_t y)
{
	uint32_t denominator = ui->canvas.height > 1U ? ui->canvas.height - 1U : 1U;
	if (y >= ui->canvas.height) y = ui->canvas.height - 1U;
	uint32_t inverse = denominator - y;
	uint32_t alpha = (((TRIAGE_BG_TOP >> 24U) & 0xFFU) * inverse + ((TRIAGE_BG_BOTTOM >> 24U) & 0xFFU) * y) / denominator;
	uint32_t red = (((TRIAGE_BG_TOP >> 16U) & 0xFFU) * inverse + ((TRIAGE_BG_BOTTOM >> 16U) & 0xFFU) * y) / denominator;
	uint32_t green = (((TRIAGE_BG_TOP >> 8U) & 0xFFU) * inverse + ((TRIAGE_BG_BOTTOM >> 8U) & 0xFFU) * y) / denominator;
	uint32_t blue = ((TRIAGE_BG_TOP & 0xFFU) * inverse + (TRIAGE_BG_BOTTOM & 0xFFU) * y) / denominator;
	return STARTUP_OPTIONS_UI_ARGB(alpha, red, green, blue);
}

static void
triage_restore_background(triage_ui_t *ui, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
	for (uint32_t row = 0U; row < height; row++) {
		startup_options_ui_canvas_fill_rect(&ui->canvas, x, y + (int32_t)row, width, 1U, triage_background_color(ui, (uint32_t)(y + (int32_t)row)));
	}
}

static uint32_t
triage_terminal_zoom_percent(uint32_t zoom)
{
	if (zoom <= 1U) {
		return 100U;
	}

	if (zoom == 2U) {
		return 125U;
	}

	return 150U;
}

static void
triage_center_text(startup_options_ui_canvas_t *canvas, int32_t center_x, int32_t y, const char *text, uint32_t color, uint32_t scale)
{
	uint32_t width = startup_options_ui_text_width(text, scale);
	startup_options_ui_draw_text(canvas, center_x - (int32_t)(width / 2U), y, text, color, scale);
}

/*
 * Vertically centre one line of text inside a control.
 *
 * StartupOptionsUI draws from the top of the line box (ascender plus descender), so the
 * inset is measured against the line height and not against the height of the
 * glyphs themselves. The recovery raster face keeps those metrics stable so
 * the same control geometry is used on every recovery boot.
 */
static int32_t
triage_text_y(int32_t box_y, uint32_t box_height, uint32_t scale)
{
	uint32_t line = startup_options_ui_line_height(scale);
	if (line == 0U || line >= box_height) {
		return box_y;
	}

	return box_y + (int32_t)((box_height - line) / 2U);
}

/*
 * Baseline-align a smaller run against a larger one drawn at `y`.
 *
 * Two runs share a baseline when their line-box tops differ by the difference
 * in their ascenders; approximating the ascender with the line height keeps the
 * two within a pixel and avoids exporting yet more font metrics.
 */
static int32_t
triage_text_y_aligned(int32_t y, uint32_t scale, uint32_t reference_scale)
{
	uint32_t line = startup_options_ui_line_height(scale);
	uint32_t reference = startup_options_ui_line_height(reference_scale);
	if (line == 0U || reference <= line) {
		return y;
	}

	return y + (int32_t)(reference - line);
}

static void
triage_mark_full(triage_ui_t *ui)
{
	ui->full_redraw = true;
	ui->dirty_buttons = 0x0FU;
	ui->dirty_continue = true;
	ui->dirty_modal = true;
	ui->dirty_disk = true;
	ui->dirty_disk_buttons = (uint8_t)((1U << (uint32_t)TRIAGE_DISK_BUTTON_COUNT) - 1U);
	ui->dirty_sheet = true;
	ui->dirty_terminal_body = true;
	ui->dirty_terminal_input = true;
	ui->damage = (triage_damage_t) { 0, 0, ui->canvas.width, ui->canvas.height, true, true };
}

static void
triage_mark_rect(triage_ui_t *ui, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
	if (ui == 0 || width == 0U || height == 0U || ui->damage.full) {
		return;
	}

	int64_t left = x < 0 ? 0 : x;
	int64_t top = y < 0 ? 0 : y;
	int64_t right = (int64_t)x + width;
	int64_t bottom = (int64_t)y + height;
	if (right > (int64_t)ui->canvas.width) {
		right = ui->canvas.width;
	}

	if (bottom > (int64_t)ui->canvas.height) {
		bottom = ui->canvas.height;
	}

	if (right <= left || bottom <= top) {
		return;
	}

	if (!ui->damage.valid) {
		ui->damage = (triage_damage_t) { (int32_t)left, (int32_t)top, (uint32_t)(right - left), (uint32_t)(bottom - top), false, true };
		return;
	}

	int64_t old_right = (int64_t)ui->damage.x + ui->damage.width;
	int64_t old_bottom = (int64_t)ui->damage.y + ui->damage.height;
	if (left > ui->damage.x) {
		left = ui->damage.x;
	}

	if (top > ui->damage.y) {
		top = ui->damage.y;
	}

	if (right < old_right) {
		right = old_right;
	}

	if (bottom < old_bottom) {
		bottom = old_bottom;
	}

	ui->damage.x = (int32_t)left;
	ui->damage.y = (int32_t)top;
	ui->damage.width = (uint32_t)(right - left);
	ui->damage.height = (uint32_t)(bottom - top);
}

static void
triage_mark_button(triage_ui_t *ui, int32_t index)
{
	if (index < 0 || index >= 4) {
		return;
	}

	ui->dirty_buttons |= (uint8_t)(1U << (uint32_t)index);
	triage_button_t *button = &ui->buttons[(uint32_t)index];
	triage_mark_rect(ui, button->x, button->y, button->width, button->height);
}

static void
triage_mark_continue(triage_ui_t *ui)
{
	ui->dirty_continue = true;
	triage_mark_rect(ui, ui->continue_button_x, ui->continue_button_y, ui->continue_button_width, ui->continue_button_height);
}

#define TRIAGE_DISK_BUTTON_RADIUS 10U

/*
 * Position the action buttons along the bottom of the panel, right aligned.
 *
 * Layout is kept out of the drawing pass for two reasons: hit testing needs
 * these rectangles from the moment Disk Utility opens, which is before
 * anything has been drawn, and the incremental repaint path draws a single
 * button without running the rest of the panel.
 */
static void
triage_disk_layout(triage_ui_t *ui)
{
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	triage_workspace(ui, &x, &y, &width, &height);

	static const uint32_t widths[TRIAGE_DISK_BUTTON_COUNT] = { 86U, 86U, 86U, 108U, 100U };
	int32_t right = x + (int32_t)width - 28;
	int32_t row_y = y + (int32_t)height - 58;

	for (uint32_t index = (uint32_t)TRIAGE_DISK_BUTTON_COUNT; index-- != 0U; ) {
		right -= (int32_t)widths[index];
		ui->disk_buttons[index] = (triage_rect_t) { right, row_y, widths[index], 40U };
		right -= 8;
	}
}

/*
 * Repaint the Disk Utility panel.
 *
 * Everything the panel shows -- the capacity bar, the partition rows, the
 * status line -- lives inside the workspace rectangle, so the backdrop
 * gradient and the title bar above it are left alone.
 */
static void
triage_mark_disk(triage_ui_t *ui)
{
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	triage_workspace(ui, &x, &y, &width, &height);
	ui->dirty_disk = true;
	triage_mark_rect(ui, x, y, width, height);
}

/*
 * Repaint one action button.
 *
 * Hover and press change nothing but that button's own fill, and a pointer
 * crossing the row would otherwise repaint -- and present -- the entire
 * screen for every move event the compositor delivers.
 */
static void
triage_mark_disk_button(triage_ui_t *ui, uint32_t index)
{
	if (index >= (uint32_t)TRIAGE_DISK_BUTTON_COUNT) {
		return;
	}

	ui->dirty_disk_buttons |= (uint8_t)(1U << index);
	const triage_rect_t *rect = &ui->disk_buttons[index];
	triage_mark_rect(ui, rect->x, rect->y, rect->width, rect->height);
}

static void
triage_layout(triage_ui_t *ui)
{
	ui->panel_width = triage_min_u32(ui->canvas.width > 112U ? ui->canvas.width - 112U : ui->canvas.width, 600U);
	ui->panel_height = triage_min_u32(ui->canvas.height > 128U ? ui->canvas.height - 128U : ui->canvas.height, 468U);
	ui->panel_x = (int32_t)((ui->canvas.width - ui->panel_width) / 2U);
	ui->panel_y = 42 + (int32_t)((ui->canvas.height - 42U - ui->panel_height) / 2U);

	uint32_t padding = 32U;
	uint32_t row_width = ui->panel_width - padding * 2U;
	uint32_t row_height = 58U;
	uint32_t gap = 8U;
	int32_t first_y = ui->panel_y + 100;
	static const triage_action_t actions[4] = { TRIAGE_ACTION_REINSTALL, TRIAGE_ACTION_DISK_UTILITY, TRIAGE_ACTION_TERMINAL, TRIAGE_ACTION_RESTART };
	static const char *const titles[4] = { "Reinstall sevOS", "Disk Utility", "Terminal", "Restart" };
	static const char *const details[4] = { "Install a system image", "Inspect storage", "Open a recovery shell", "Restart the system" };

	for (uint32_t index = 0U; index < 4U; index++) {
		ui->buttons[index] = (triage_button_t) {
			.x = ui->panel_x + (int32_t)padding,
			.y = first_y + (int32_t)(index * (row_height + gap)),
			.width = row_width,
			.height = row_height,
			.action = actions[index],
			.title = titles[index],
			.detail = details[index]
		};
	}

	ui->continue_button_width = 95U;
	ui->continue_button_height = 26U;
	ui->continue_button_x = ui->panel_x + (int32_t)ui->panel_width - (int32_t)padding - (int32_t)ui->continue_button_width;
	ui->continue_button_y = ui->panel_y + (int32_t)ui->panel_height - 56;
}

static int32_t
triage_hit_button(const triage_ui_t *ui, int32_t x, int32_t y)
{
	for (uint32_t index = 0U; index < 4U; index++) {
		const triage_button_t *button = &ui->buttons[index];
		if (startup_options_ui_point_in_squircle(x, y, button->x, button->y, button->width, button->height, 12U)) {
			return (int32_t)index;
		}
	}

	return -1;
}

static bool
triage_continue_hit(const triage_ui_t *ui, int32_t x, int32_t y)
{
	return startup_options_ui_point_in_squircle(x, y, ui->continue_button_x, ui->continue_button_y, ui->continue_button_width, ui->continue_button_height, 11U);
}

static bool
triage_modal_button_hit(const triage_ui_t *ui, int32_t x, int32_t y)
{
	return startup_options_ui_point_in_squircle(x, y, ui->modal_button_x, ui->modal_button_y, ui->modal_button_width, ui->modal_button_height, 11U);
}

static void
triage_draw_static(triage_ui_t *ui)
{
	startup_options_ui_canvas_fill_vertical_gradient(&ui->canvas, TRIAGE_BG_TOP, TRIAGE_BG_BOTTOM);
	startup_options_ui_canvas_fill_rect(&ui->canvas, 0, 0, ui->canvas.width, TRIAGE_BAR_HEIGHT, TRIAGE_BAR);
	int32_t bar_text_y = triage_text_y(0, TRIAGE_BAR_HEIGHT, 2U);
	startup_options_ui_draw_text(&ui->canvas, 18, bar_text_y, "triageOS", TRIAGE_TEXT, 2U);
	startup_options_ui_draw_text(&ui->canvas, 124, bar_text_y, "Recovery", TRIAGE_MUTED, 2U);
	startup_options_ui_canvas_stroke_squircle(&ui->canvas, ui->panel_x, ui->panel_y, ui->panel_width, ui->panel_height, 20U, 1U, TRIAGE_BORDER, TRIAGE_PANEL);
	triage_center_text(&ui->canvas, ui->panel_x + (int32_t)(ui->panel_width / 2U), ui->panel_y + 30, "sevOS Recovery", TRIAGE_TEXT, 3U);
	triage_center_text(&ui->canvas, ui->panel_x + (int32_t)(ui->panel_width / 2U), ui->panel_y + 66, "Choose a recovery tool.", TRIAGE_MUTED, 2U);
	startup_options_ui_draw_text(
		&ui->canvas,
		ui->panel_x + 38,
		triage_text_y(ui->continue_button_y, ui->continue_button_height, 2U),
		"Recovery",
		TRIAGE_MUTED,
		2U
	);
}

static void
triage_draw_button(triage_ui_t *ui, uint32_t index)
{
	triage_button_t *button = &ui->buttons[index];
	bool selected = (int32_t)index == ui->selected_button;
	bool hovered = (int32_t)index == ui->hovered_button;
	bool pressed = (int32_t)index == ui->pressed_button;
	uint32_t fill = selected ? TRIAGE_ROW_SELECTED : (hovered ? TRIAGE_ROW_HOVER : TRIAGE_ROW);
	if (pressed) {
		fill = TRIAGE_ROW_PRESSED;
	}

	startup_options_ui_canvas_fill_rect(&ui->canvas, button->x, button->y, button->width, button->height, TRIAGE_PANEL);
	startup_options_ui_canvas_stroke_squircle(&ui->canvas, button->x, button->y, button->width, button->height, 12U, selected ? 2U : 1U, selected ? TRIAGE_ACCENT : TRIAGE_BORDER, fill);
	/* Title over detail: centre the pair as one block so the row is not bottom-heavy. */
	uint32_t title_line = startup_options_ui_line_height(2U);
	uint32_t detail_line = startup_options_ui_line_height(1U);
	uint32_t block = title_line + TRIAGE_ROW_TEXT_GAP + detail_line;
	int32_t title_y = button->y + (button->height > block ? (int32_t)((button->height - block) / 2U) : 0);
	startup_options_ui_draw_text(&ui->canvas, button->x + 20, title_y, button->title, TRIAGE_TEXT, 2U);
	startup_options_ui_draw_text(
		&ui->canvas,
		button->x + 20,
		title_y + (int32_t)(title_line + TRIAGE_ROW_TEXT_GAP),
		button->detail,
		TRIAGE_MUTED,
		1U
	);
}

static void
triage_draw_continue(triage_ui_t *ui)
{
	uint32_t color = TRIAGE_CONTINUE;
	if (ui->continue_button_pressed) {
		color = TRIAGE_CONTINUE_PRESSED;
	} else if (ui->continue_button_hovered) {
		color = TRIAGE_CONTINUE_HOVER;
	}

	startup_options_ui_canvas_fill_rect(&ui->canvas, ui->continue_button_x, ui->continue_button_y, ui->continue_button_width, ui->continue_button_height, TRIAGE_PANEL);
	startup_options_ui_canvas_fill_squircle(&ui->canvas, ui->continue_button_x, ui->continue_button_y, ui->continue_button_width, ui->continue_button_height, 11U, color);
	triage_center_text(
		&ui->canvas,
		ui->continue_button_x + (int32_t)(ui->continue_button_width / 2U),
		triage_text_y(ui->continue_button_y, ui->continue_button_height, 2U),
		"Continue",
		TRIAGE_CONTINUE_TEXT,
		2U
	);
}

static const char *
triage_modal_title(triage_action_t action)
{
	if (action == TRIAGE_ACTION_REINSTALL) {
		return "Reinstall sevOS";
	}

	if (action == TRIAGE_ACTION_DISK_UTILITY) {
		return "Disk Utility";
	}

	if (action == TRIAGE_ACTION_RESTART) {
		return "Restart failed";
	}

	return "Recovery";
}

static const char *
triage_modal_message(triage_action_t action)
{
	if (action == TRIAGE_ACTION_REINSTALL) {
		return "Installer support is not available yet.";
	}

	if (action == TRIAGE_ACTION_DISK_UTILITY) {
		return "Use Terminal for filesystem tools.";
	}

	if (action == TRIAGE_ACTION_RESTART) {
		return "The reset request failed.";
	}

	return "This tool is unavailable.";
}

static void
triage_draw_modal_button(triage_ui_t *ui)
{
	uint32_t color = ui->modal_button_pressed ? TRIAGE_ACCENT_PRESSED : (ui->modal_button_hovered ? TRIAGE_ACCENT_HOVER : TRIAGE_ACCENT);
	startup_options_ui_canvas_fill_rect(&ui->canvas, ui->modal_button_x, ui->modal_button_y, ui->modal_button_width, ui->modal_button_height, TRIAGE_MODAL);
	startup_options_ui_canvas_fill_squircle(&ui->canvas, ui->modal_button_x, ui->modal_button_y, ui->modal_button_width, ui->modal_button_height, 10U, color);
	triage_center_text(
		&ui->canvas,
		ui->modal_button_x + (int32_t)(ui->modal_button_width / 2U),
		triage_text_y(ui->modal_button_y, ui->modal_button_height, 2U),
		"Done",
		TRIAGE_TEXT,
		2U
	);
}

static void
triage_draw_modal(triage_ui_t *ui)
{
	startup_options_ui_canvas_fill_rect(&ui->canvas, 0, 0, ui->canvas.width, ui->canvas.height, TRIAGE_MODAL_DIM);
	uint32_t width = triage_min_u32(ui->canvas.width - 80U, 470U);
	uint32_t height = 210U;
	int32_t x = (int32_t)((ui->canvas.width - width) / 2U);
	int32_t y = (int32_t)((ui->canvas.height - height) / 2U);
	startup_options_ui_canvas_stroke_squircle(&ui->canvas, x, y, width, height, 18U, 1U, TRIAGE_BORDER, TRIAGE_MODAL);
	const char *title = ui->modal_custom ? ui->modal_title : triage_modal_title(ui->modal_action);
	const char *message = ui->modal_custom ? ui->modal_message : triage_modal_message(ui->modal_action);
	triage_center_text(&ui->canvas, x + (int32_t)(width / 2U), y + 34, title, TRIAGE_TEXT, 3U);
	triage_center_text(&ui->canvas, x + (int32_t)(width / 2U), y + 88, message, TRIAGE_MUTED, 1U);
	ui->modal_button_width = 108U;
	ui->modal_button_height = 40U;
	ui->modal_button_x = x + (int32_t)((width - ui->modal_button_width) / 2U);
	ui->modal_button_y = y + (int32_t)height - 54;
	triage_draw_modal_button(ui);
}

static void
triage_terminal_push(triage_ui_t *ui, const char *text)
{
	if (ui == 0 || text == 0) {
		return;
	}

	if (ui->terminal_line_count == TRIAGE_TERMINAL_LINES) {
		for (uint32_t line = 1U; line < TRIAGE_TERMINAL_LINES; line++) {
			triage_copy(ui->terminal_lines[line - 1U], TRIAGE_TERMINAL_LINE_MAX, ui->terminal_lines[line]);
		}

		ui->terminal_line_count--;
	}

	triage_copy(ui->terminal_lines[ui->terminal_line_count++], TRIAGE_TERMINAL_LINE_MAX, text);
	ui->dirty_terminal_body = true;
}

static void
triage_shell_output(void *context, const char *text)
{
	triage_terminal_push(context, text);
}

static void
triage_terminal_enter(triage_ui_t *ui)
{
	ui->terminal_active = true;
	ui->disk_active = false;
	ui->terminal_input_length = 0U;
	ui->terminal_input[0] = '\0';
	ui->terminal_history_index = -1;
	triage_mark_full(ui);
}

static char
triage_key_character(uint32_t code, uint32_t modifiers)
{
	bool shift = (modifiers & STARTUP_OPTIONS_UI_MOD_SHIFT) != 0U;
	bool caps = (modifiers & STARTUP_OPTIONS_UI_MOD_CAPS) != 0U;
	static const char row1[] = "qwertyuiop";
	static const char row2[] = "asdfghjkl";
	static const char row3[] = "zxcvbnm";
	char value = '\0';
	if (code >= 16U && code <= 25U) {
		value = row1[code - 16U];
	} else if (code >= 30U && code <= 38U) {
		value = row2[code - 30U];
	} else if (code >= 44U && code <= 50U) {
		value = row3[code - 44U];
	}

	if (value != '\0') {
		return shift != caps ? (char)(value - 'a' + 'A') : value;
	}

	if (code >= 2U && code <= 11U) {
		static const char normal[] = "1234567890";
		static const char shifted[] = "!@#$%^&*()";
		return shift ? shifted[code - 2U] : normal[code - 2U];
	}

	switch (code) {
	case 12U: return shift ? '_' : '-';
	case 13U: return shift ? '+' : '=';
	case 26U: return shift ? '{' : '[';
	case 27U: return shift ? '}' : ']';
	case 39U: return shift ? ':' : ';';
	case 40U: return shift ? '"' : '\'';
	case 41U: return shift ? '~' : '`';
	case 43U: return shift ? '|' : '\\';
	case 51U: return shift ? '<' : ',';
	case 52U: return shift ? '>' : '.';
	case 53U: return shift ? '?' : '/';
	case 57U: return ' ';
	default: return '\0';
	}
}

static void
triage_terminal_history_add(triage_ui_t *ui)
{
	if (ui->terminal_input_length == 0U) {
		return;
	}

	if (ui->terminal_history_count == TRIAGE_TERMINAL_HISTORY) {
		for (uint32_t i = 1U; i < TRIAGE_TERMINAL_HISTORY; i++) {
			triage_copy(ui->terminal_history[i - 1U], TRIAGE_TERMINAL_INPUT_MAX, ui->terminal_history[i]);
		}

		ui->terminal_history_count--;
	}

	triage_copy(ui->terminal_history[ui->terminal_history_count++], TRIAGE_TERMINAL_INPUT_MAX, ui->terminal_input);
	ui->terminal_history_index = -1;
}

static void
triage_terminal_history_move(triage_ui_t *ui, int32_t direction)
{
	if (ui->terminal_history_count == 0U) {
		return;
	}

	if (direction < 0) {
		if (ui->terminal_history_index < 0) {
			ui->terminal_history_index = (int32_t)ui->terminal_history_count - 1;
		} else if (ui->terminal_history_index > 0) {
			ui->terminal_history_index--;
		}
	} else {
		if (ui->terminal_history_index < 0) {
			return;
		}

		ui->terminal_history_index++;
		if (ui->terminal_history_index >= (int32_t)ui->terminal_history_count) {
			ui->terminal_history_index = -1;
			ui->terminal_input[0] = '\0';
			ui->terminal_input_length = 0U;
			ui->dirty_terminal_input = true;
			return;
		}
	}

	triage_copy(ui->terminal_input, sizeof(ui->terminal_input), ui->terminal_history[(uint32_t)ui->terminal_history_index]);
	ui->terminal_input_length = triage_length(ui->terminal_input);
	ui->dirty_terminal_input = true;
}

static void
triage_terminal_execute(triage_ui_t *ui)
{
	char command[TRIAGE_TERMINAL_INPUT_MAX];
	triage_copy(command, sizeof(command), ui->terminal_input);
	if (command[0] != '\0') {
		char prompt[TRIAGE_TERMINAL_LINE_MAX];
		prompt[0] = '#'; prompt[1] = ' '; prompt[2] = '\0';
		uint32_t out = 2U;
		for (uint32_t i = 0U; command[i] != '\0' && out + 1U < sizeof(prompt); i++) {
			prompt[out++] = command[i];
		}

		prompt[out] = '\0';
		triage_terminal_push(ui, prompt);
		triage_terminal_history_add(ui);
	}

	triage_shell_result_t result = triage_shell_execute(&ui->shell, command);
	ui->terminal_input_length = 0U;
	ui->terminal_input[0] = '\0';
	ui->dirty_terminal_input = true;
	if (result == TRIAGE_SHELL_CLEAR) {
		ui->terminal_line_count = 0U;
		ui->dirty_terminal_body = true;
	}

	if (result == TRIAGE_SHELL_EXIT) {
		ui->terminal_active = false;
		triage_mark_full(ui);
	}
}

static void
triage_draw_terminal_body(triage_ui_t *ui)
{
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	triage_workspace(ui, &x, &y, &width, &height);

	startup_options_ui_canvas_fill_rect(&ui->canvas, x, y, width, height, TRIAGE_TERMINAL);
	startup_options_ui_canvas_stroke_squircle(&ui->canvas, x, y, width, height, 10U, 1U, TRIAGE_TERMINAL_BORDER, TRIAGE_TERMINAL);
	/*
	 * Title over hint, centred as one block in the band above the divider, with
	 * the zoom readout sharing the title's baseline.
	 */
	uint32_t title_line = startup_options_ui_line_height(2U);
	uint32_t hint_line = startup_options_ui_line_height(1U);
	uint32_t header_block = title_line + TRIAGE_TERMINAL_HEADER_GAP + hint_line;
	int32_t title_y = y
		+ (TRIAGE_TERMINAL_HEADER_HEIGHT > header_block
			? (int32_t)((TRIAGE_TERMINAL_HEADER_HEIGHT - header_block) / 2U)
			: 0);
	startup_options_ui_draw_text(&ui->canvas, x + 20, title_y, "Terminal", TRIAGE_TERMINAL_TEXT, 2U);

	char zoom[16];
	triage_u64(zoom, sizeof(zoom), triage_terminal_zoom_percent(ui->terminal_zoom));
	triage_append(zoom, sizeof(zoom), "%");
	uint32_t zoom_width = startup_options_ui_text_width(zoom, 1U);
	startup_options_ui_draw_text(
		&ui->canvas,
		x + (int32_t)width - 20 - (int32_t)zoom_width,
		triage_text_y_aligned(title_y, 1U, 2U),
		zoom,
		TRIAGE_TERMINAL_MUTED,
		1U
	);
	startup_options_ui_draw_text(
		&ui->canvas,
		x + 20,
		title_y + (int32_t)(title_line + TRIAGE_TERMINAL_HEADER_GAP),
		"Ctrl+-  Ctrl++  Ctrl+0",
		TRIAGE_TERMINAL_MUTED,
		1U
	);
	startup_options_ui_canvas_fill_rect(
		&ui->canvas, x + 18, y + (int32_t)TRIAGE_TERMINAL_HEADER_HEIGHT, width - 36U, 1U, TRIAGE_TERMINAL_BORDER
	);

	uint32_t line_height = startup_options_ui_mono_line_height(ui->terminal_zoom);
	if (line_height == 0U) {
		line_height = 22U;
	}

	uint32_t input_height = line_height + 18U;
	uint32_t body_height = height > 82U + input_height ? height - 82U - input_height : 0U;
	uint32_t visible = line_height == 0U ? 0U : body_height / line_height;
	if (visible > TRIAGE_TERMINAL_LINES) {
		visible = TRIAGE_TERMINAL_LINES;
	}

	uint32_t start = ui->terminal_line_count > visible ? ui->terminal_line_count - visible : 0U;
	uint32_t row = 0U;
	for (uint32_t line = start; line < ui->terminal_line_count; line++, row++) {
		startup_options_ui_draw_mono_text(
			&ui->canvas,
			x + 20,
			y + 78 + (int32_t)(row * line_height),
			ui->terminal_lines[line],
			TRIAGE_TERMINAL_TEXT,
			ui->terminal_zoom
		);
	}

	ui->dirty_terminal_input = true;
}

static void
triage_draw_terminal_input(triage_ui_t *ui)
{
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	triage_workspace(ui, &x, &y, &width, &height);
	uint32_t line_height = startup_options_ui_mono_line_height(ui->terminal_zoom);
	if (line_height == 0U) {
		line_height = 22U;
	}

	uint32_t input_height = line_height + 18U;
	int32_t input_y = y + (int32_t)height - (int32_t)input_height;

	startup_options_ui_canvas_fill_rect(&ui->canvas, x + 18, input_y, width - 36U, input_height, TRIAGE_TERMINAL);
	startup_options_ui_canvas_fill_rect(&ui->canvas, x + 18, input_y, width - 36U, 1U, TRIAGE_TERMINAL_BORDER);
	startup_options_ui_draw_mono_text(&ui->canvas, x + 20, input_y + 9, "# ", TRIAGE_TERMINAL_TEXT, ui->terminal_zoom);
	uint32_t prompt_width = startup_options_ui_mono_text_width("# ", ui->terminal_zoom);
	startup_options_ui_draw_mono_text(&ui->canvas, x + 20 + (int32_t)prompt_width, input_y + 9, ui->terminal_input, TRIAGE_TERMINAL_TEXT, ui->terminal_zoom);
	int32_t cursor_x = x + 20 + (int32_t)prompt_width + (int32_t)startup_options_ui_mono_text_width(ui->terminal_input, ui->terminal_zoom);
	startup_options_ui_canvas_fill_rect(&ui->canvas, cursor_x, input_y + 9, 2U, line_height > 4U ? line_height - 4U : line_height, TRIAGE_TERMINAL_TEXT);
	triage_mark_rect(ui, x + 18, input_y, width - 36U, input_height);
}

static void
triage_disk_refresh(triage_ui_t *ui)
{
	ui->disk_info = (recovery_block_info_t) { 0 };
	ui->disk_health = (recovery_block_health_info_t) { 0 };
	ui->disk_health_valid = false;
	ui->disk_layout = (recovery_block_layout_info_t) { 0 };
	ui->disk_space = (recovery_fs_space_info_t) { 0 };
	ui->disk_space_valid = false;
	ui->disk_mounted = false;
	ui->disk_mount_path[0] = '\0';
	ui->disk_partition_count = 0U;

	if (recovery_block_info(0U, &ui->disk_info) < 0) {
		return;
	}

	for (uint32_t index = 0U; ; index++) {
		recovery_fs_mount_info_t mount;
		if (recovery_fs_mount_info(index, &mount) < 0) break;
		if (mount.device_index != 0U && !triage_equal(mount.device, ui->disk_info.name)) continue;

		ui->disk_mounted = true;
		triage_copy(ui->disk_mount_path, sizeof(ui->disk_mount_path), mount.path);
		ui->disk_space_valid = recovery_fs_space_info(mount.path, &ui->disk_space) >= 0;
		break;
	}

	ui->disk_health_valid = recovery_block_health_info(0U, &ui->disk_health) >= 0;

	if (recovery_block_layout_info(0U, &ui->disk_layout) < 0) {
		ui->disk_layout.scheme = RECOVERY_BLOCK_LAYOUT_RAW;
		ui->disk_layout.partition_count = 0U;
		return;
	}

	uint32_t count = ui->disk_layout.partition_count;
	if (count > TRIAGE_DISK_PARTITIONS_MAX) {
		count = TRIAGE_DISK_PARTITIONS_MAX;
	}

	for (uint32_t index = 0U; index < count; index++) {
		if (recovery_block_partition_info(0U, index, &ui->disk_partitions[ui->disk_partition_count]) >= 0) {
			ui->disk_partition_count++;
		}
	}

	if (ui->disk_partition_count == 0U) {
		ui->disk_selected_partition = 0U;
	} else if (ui->disk_selected_partition >= ui->disk_partition_count) {
		ui->disk_selected_partition = ui->disk_partition_count - 1U;
	}
}

static bool
triage_disk_hit(int32_t x, int32_t y, int32_t bx, int32_t by, uint32_t width, uint32_t height)
{
	return x >= bx && y >= by && x < bx + (int32_t)width && y < by + (int32_t)height;
}

static bool
triage_disk_partition_hit(triage_ui_t *ui, int32_t x, int32_t y, uint32_t *index_out)
{
	if (ui == 0 || index_out == 0 || ui->disk_partition_count == 0U) {
		return false;
	}

	int32_t workspace_x;
	int32_t workspace_y;
	uint32_t workspace_width;
	uint32_t workspace_height;
	triage_workspace(ui, &workspace_x, &workspace_y, &workspace_width, &workspace_height);
	(void)workspace_height;

	int32_t detail_y = workspace_y + 286;
	uint32_t row_width = workspace_width - 56U;

	for (uint32_t index = 0U; index < ui->disk_partition_count; index++) {
		int32_t row_y = detail_y + (int32_t)(index * 34U) - 5;
		if (!triage_disk_hit(x, y, workspace_x + 22, row_y, row_width + 12U, 30U)) {
			continue;
		}

		*index_out = index;
		return true;
	}

	return false;
}

static void
triage_disk_set_status(triage_ui_t *ui, const char *text)
{
	triage_copy(ui->disk_status, sizeof(ui->disk_status), text);
}

static void
triage_disk_set_error(triage_ui_t *ui, const char *operation, int64_t result)
{
	char status[96];
	status[0] = '\0';
	triage_append(status, sizeof(status), operation);
	triage_append(status, sizeof(status), ": ");
	triage_append(status, sizeof(status), recovery_error_name(result));
	triage_disk_set_status(ui, status);
}

static void
triage_disk_cancel_destructive(triage_ui_t *ui)
{
	ui->disk_initialize_armed = false;
	ui->disk_delete_armed = false;
}

static void
triage_disk_toggle_mount(triage_ui_t *ui)
{
	triage_disk_cancel_destructive(ui);

	int64_t result;
	if (ui->disk_mounted) {
		result = recovery_fs_unmount(ui->disk_mount_path);
		triage_disk_set_status(ui, result < 0 ? "Unable to unmount sevOS System." : "sevOS System unmounted.");
	} else if (ui->disk_layout.scheme == RECOVERY_BLOCK_LAYOUT_RAW) {
		result = recovery_fs_mount("ext4", 0U, "/disk");
		triage_disk_set_status(ui, result < 0 ? "Unable to mount sevOS System." : "sevOS System mounted at /disk.");
	} else {
		uint32_t system_device;
		if (!triage_device_index(TRIAGE_SYSTEM_DEVICE, &system_device)) {
			triage_disk_set_status(ui, "Partition-backed mounts are not available yet.");
			triage_mark_disk(ui);
			return;
		}

		result = recovery_fs_mount("ext4", system_device, "/disk");
		triage_disk_set_status(ui, result < 0 ? "Unable to mount sevOS System." : "sevOS System mounted at /disk.");
	}

	triage_disk_refresh(ui);
	triage_mark_disk(ui);
}

static void
triage_disk_verify(triage_ui_t *ui)
{
	triage_disk_cancel_destructive(ui);
	int64_t result = recovery_block_verify(0U);
	triage_disk_refresh(ui);

	if (result < 0) {
		triage_disk_set_error(ui, "Verification failed", result);
	} else {
		triage_disk_set_status(ui, "Verification passed; sampled sectors are readable.");
	}

	triage_mark_disk(ui);
}

/*
 * Add Partition sheet.
 *
 * Disk Utility's partition menu used to add a partition on the spot with a
 * fixed name and whatever space happened to be left.  The sheet below is what
 * turns that into a choice: a name, a role, and a size, previewed against the
 * partition map before anything is written.
 *
 * Geometry is computed once per event by triage_sheet_layout() and stored on
 * the sheet, so drawing and hit testing read the same rectangles instead of
 * each recomputing them from the same constants.
 */
#define TRIAGE_SHEET_WIDTH 560U
#define TRIAGE_SHEET_HEIGHT 400U
#define TRIAGE_SHEET_PADDING 24U
#define TRIAGE_SHEET_FIELD_HEIGHT 30U
#define TRIAGE_SHEET_RADIUS 16U
#define TRIAGE_SHEET_CONTROL_RADIUS 9U
#define TRIAGE_SHEET_BUTTON_RADIUS 10U
#define TRIAGE_SHEET_ALIGNMENT_SECTORS 2048ULL
#define TRIAGE_SHEET_GPT_RESERVE_SECTORS 34ULL
#define TRIAGE_SHEET_MINIMUM_BYTES (1024ULL * 1024ULL)

static uint64_t
triage_sheet_align_up(uint64_t value, uint64_t alignment)
{
	uint64_t remainder = value % alignment;
	return remainder == 0ULL ? value : value + (alignment - remainder);
}

static uint64_t
triage_parse_u64(const char *text)
{
	uint64_t value = 0ULL;
	if (text == 0) {
		return 0ULL;
	}

	for (uint32_t index = 0U; text[index] != '\0'; index++) {
		if (text[index] < '0' || text[index] > '9') {
			break;
		}

		if (value > 1000000000000ULL) {
			break;
		}

		value = value * 10ULL + (uint64_t)(text[index] - '0');
	}

	return value;
}

/*
 * Human sizes with one decimal.
 *
 * triage_format_bytes() truncates to whole units, which is fine for a static
 * capacity readout but makes a size slider look stuck: dragging across a third
 * of a gibibyte would not change the number at all.
 */
static void
triage_format_size(char *buffer, uint32_t capacity, uint64_t bytes)
{
	if (buffer == 0 || capacity == 0U) {
		return;
	}

	uint64_t unit = 1024ULL * 1024ULL * 1024ULL;
	const char *suffix = " GiB";
	if (bytes < 1024ULL * 1024ULL) {
		triage_format_bytes(buffer, capacity, bytes);
		return;
	}

	if (bytes < unit) {
		unit = 1024ULL * 1024ULL;
		suffix = " MiB";
	}

	char whole[24];
	char tenths[4];
	triage_u64(whole, sizeof(whole), bytes / unit);
	triage_u64(tenths, sizeof(tenths), ((bytes % unit) * 10ULL) / unit);
	buffer[0] = '\0';
	triage_append(buffer, capacity, whole);
	triage_append(buffer, capacity, ".");
	triage_append(buffer, capacity, tenths);
	triage_append(buffer, capacity, suffix);
}

static const char *
triage_sheet_role_name(uint32_t role)
{
	if (role == (uint32_t)RECOVERY_BLOCK_PARTITION_ROLE_SYSTEM) {
		return "sevOS System";
	}

	if (role == (uint32_t)RECOVERY_BLOCK_PARTITION_ROLE_EFI) {
		return "EFI System";
	}

	return "sevOS Data";
}

/*
 * Where the kernel would place a partition of `requested` sectors.
 *
 * This mirrors partition_find_space(): first gap that fits when a size is
 * given, largest gap when the caller asked for everything.  Mirroring it is
 * what lets the sheet preview the real outcome and refuse an impossible size
 * before the partition table is touched, rather than reporting NO_SPACE after
 * the fact.
 */
static bool
triage_sheet_place(const triage_ui_t *ui, uint64_t requested, uint64_t *start_out, uint64_t *count_out)
{
	const triage_disk_sheet_t *sheet = &ui->disk_sheet;
	if (start_out == 0 || count_out == 0 || sheet->disk_sectors == 0ULL) {
		return false;
	}

	uint64_t first[TRIAGE_DISK_PARTITIONS_MAX];
	uint64_t last[TRIAGE_DISK_PARTITIONS_MAX];
	uint32_t count = 0U;

	for (uint32_t index = 0U; index < ui->disk_partition_count; index++) {
		const recovery_block_partition_info_t *partition = &ui->disk_partitions[index];
		if (partition->sector_count == 0ULL) {
			continue;
		}

		uint64_t start = partition->start_sector;
		uint64_t end = start + partition->sector_count - 1ULL;
		uint32_t slot = count;
		while (slot != 0U && first[slot - 1U] > start) {
			first[slot] = first[slot - 1U];
			last[slot] = last[slot - 1U];
			slot--;
		}

		first[slot] = start;
		last[slot] = end;
		count++;
	}

	uint64_t reserve = ui->disk_layout.scheme == RECOVERY_BLOCK_LAYOUT_GPT
		? TRIAGE_SHEET_GPT_RESERVE_SECTORS
		: 1ULL;
	if (sheet->disk_sectors <= reserve) {
		return false;
	}

	uint64_t last_usable = sheet->disk_sectors - reserve;
	uint64_t cursor = TRIAGE_SHEET_ALIGNMENT_SECTORS;
	uint64_t best_start = 0ULL;
	uint64_t best_count = 0ULL;

	for (uint32_t index = 0U; index <= count; index++) {
		bool final_gap = index == count;
		uint64_t gap_last = final_gap ? last_usable : (first[index] == 0ULL ? 0ULL : first[index] - 1ULL);

		if (final_gap || first[index] > cursor) {
			if (gap_last >= cursor) {
				uint64_t gap = gap_last - cursor + 1ULL;
				if (requested != 0ULL && gap >= requested) {
					*start_out = cursor;
					*count_out = requested;
					return true;
				}

				if (gap > best_count) {
					best_start = cursor;
					best_count = gap;
				}
			}
		}

		if (final_gap) {
			break;
		}

		uint64_t next = last[index] + 1ULL;
		if (next > cursor) {
			cursor = triage_sheet_align_up(next, TRIAGE_SHEET_ALIGNMENT_SECTORS);
		}

		if (cursor > last_usable) {
			break;
		}
	}

	if (requested != 0ULL || best_count == 0ULL) {
		*start_out = best_start;
		*count_out = best_count;
		return false;
	}

	*start_out = best_start;
	*count_out = best_count;
	return true;
}

static uint64_t
triage_sheet_unit_bytes(const triage_disk_sheet_t *sheet)
{
	return sheet->gibibytes ? 1024ULL * 1024ULL * 1024ULL : 1024ULL * 1024ULL;
}

/* Byte size the fields currently describe, before it is clamped to a real gap. */
static uint64_t
triage_sheet_requested_bytes(const triage_disk_sheet_t *sheet)
{
	if (sheet->use_all) {
		return sheet->free_sectors * sheet->sector_size;
	}

	return triage_parse_u64(sheet->size_text) * triage_sheet_unit_bytes(sheet);
}

static uint64_t
triage_sheet_requested_sectors(const triage_disk_sheet_t *sheet)
{
	if (sheet->use_all) {
		return sheet->free_sectors;
	}

	uint64_t sector_size = sheet->sector_size != 0ULL ? sheet->sector_size : 512ULL;
	uint64_t bytes = triage_sheet_requested_bytes(sheet);
	return (bytes + sector_size - 1ULL) / sector_size;
}

static bool
triage_sheet_size_valid(const triage_disk_sheet_t *sheet)
{
	uint64_t sectors = triage_sheet_requested_sectors(sheet);
	uint64_t sector_size = sheet->sector_size != 0ULL ? sheet->sector_size : 512ULL;
	if (sectors == 0ULL || sectors > sheet->free_sectors) {
		return false;
	}

	return sectors * sector_size >= TRIAGE_SHEET_MINIMUM_BYTES;
}

/* Write `bytes` back into the size field, in whichever unit is selected. */
static void
triage_sheet_set_bytes(triage_disk_sheet_t *sheet, uint64_t bytes)
{
	uint64_t unit = triage_sheet_unit_bytes(sheet);
	uint64_t value = (bytes + unit / 2ULL) / unit;
	if (value == 0ULL) {
		value = 1ULL;
	}

	triage_u64(sheet->size_text, sizeof(sheet->size_text), value);
	sheet->size_length = triage_length(sheet->size_text);
}

/*
 * Repaint the sheet alone.
 *
 * The backdrop behind an open sheet never changes, and its drop shadow is 26
 * blended squircle layers -- redrawing either one per keystroke costs more
 * than everything else in the frame put together.  Sheet-internal state
 * therefore marks only the sheet, so the dim, the shadow, the Disk Utility
 * panel underneath and the full-screen present are all skipped.
 */
static void
triage_mark_sheet(triage_ui_t *ui)
{
	ui->dirty_sheet = true;
	triage_mark_rect(ui, ui->disk_sheet.frame.x, ui->disk_sheet.frame.y, ui->disk_sheet.frame.width, ui->disk_sheet.frame.height);
}

static void
triage_sheet_open(triage_ui_t *ui)
{
	triage_disk_sheet_t *sheet = &ui->disk_sheet;
	*sheet = (triage_disk_sheet_t) { 0 };
	sheet->active = true;
	sheet->sector_size = ui->disk_info.sector_size != 0U ? ui->disk_info.sector_size : 512ULL;
	sheet->disk_sectors = ui->disk_info.sector_count;
	sheet->role = (uint32_t)RECOVERY_BLOCK_PARTITION_ROLE_DATA;
	sheet->focus = TRIAGE_SHEET_CONTROL_NAME;
	triage_copy(sheet->name, sizeof(sheet->name), "Untitled");
	sheet->name_length = triage_length(sheet->name);

	sheet->used_sectors = 0ULL;
	for (uint32_t index = 0U; index < ui->disk_partition_count; index++) {
		sheet->used_sectors += ui->disk_partitions[index].sector_count;
	}

	uint64_t start;
	uint64_t largest;
	sheet->free_sectors = triage_sheet_place(ui, 0ULL, &start, &largest) ? largest : 0ULL;

	uint64_t free_bytes = sheet->free_sectors * sheet->sector_size;
	sheet->gibibytes = free_bytes >= 4ULL * 1024ULL * 1024ULL * 1024ULL;
	triage_sheet_set_bytes(sheet, free_bytes / 2ULL);

	if (sheet->free_sectors == 0ULL) {
		triage_copy(sheet->message, sizeof(sheet->message), "No free space remains on this disk.");
	} else if (ui->disk_layout.scheme == RECOVERY_BLOCK_LAYOUT_MBR) {
		triage_copy(sheet->message, sizeof(sheet->message), "MBR partition maps do not store names.");
	}

	triage_mark_full(ui);
}

static void
triage_sheet_close(triage_ui_t *ui)
{
	ui->disk_sheet.active = false;
	ui->disk_sheet.dragging = false;
	ui->disk_sheet.pressed = TRIAGE_SHEET_CONTROL_NONE;
	ui->disk_sheet.hovered = TRIAGE_SHEET_CONTROL_NONE;
	triage_mark_full(ui);
}

static void
triage_sheet_apply(triage_ui_t *ui)
{
	triage_disk_sheet_t *sheet = &ui->disk_sheet;

	if (sheet->free_sectors == 0ULL) {
		triage_copy(sheet->message, sizeof(sheet->message), "No free space remains on this disk.");
		triage_mark_sheet(ui);
		return;
	}

	if (!sheet->use_all && !triage_sheet_size_valid(sheet)) {
		triage_copy(sheet->message, sizeof(sheet->message), "Choose a size between 1 MiB and the free space shown.");
		triage_mark_sheet(ui);
		return;
	}

	recovery_block_partition_info_t partition;
	int64_t result = recovery_block_partition_create(
		0U,
		sheet->use_all ? 0ULL : triage_sheet_requested_sectors(sheet),
		sheet->name[0] != '\0' ? sheet->name : "Untitled",
		(recovery_block_partition_role_t)sheet->role,
		&partition
	);

	if (result < 0) {
		sheet->message[0] = '\0';
		triage_append(sheet->message, sizeof(sheet->message), "Add partition failed: ");
		triage_append(sheet->message, sizeof(sheet->message), recovery_error_name(result));
		triage_mark_sheet(ui);
		return;
	}

	triage_sheet_close(ui);
	triage_disk_refresh(ui);
	ui->disk_selected_partition = partition.index;

	char status[96];
	char size[32];
	triage_format_size(size, sizeof(size), partition.sector_count * (uint64_t)ui->disk_info.sector_size);
	status[0] = '\0';
	triage_append(status, sizeof(status), "Created ");
	triage_append(status, sizeof(status), partition.name[0] != '\0' ? partition.name : "Untitled");
	triage_append(status, sizeof(status), " (");
	triage_append(status, sizeof(status), size);
	triage_append(status, sizeof(status), "). Restart recovery to publish it as a block device.");
	triage_disk_set_status(ui, status);
	triage_mark_full(ui);
}

static triage_rect_t
triage_rect(int32_t x, int32_t y, uint32_t width, uint32_t height)
{
	return (triage_rect_t) { x, y, width, height };
}

static bool
triage_rect_hit(const triage_rect_t *rect, int32_t x, int32_t y)
{
	return x >= rect->x && y >= rect->y && x < rect->x + (int32_t)rect->width && y < rect->y + (int32_t)rect->height;
}

static void
triage_sheet_layout(triage_ui_t *ui)
{
	triage_disk_sheet_t *sheet = &ui->disk_sheet;
	uint32_t width = triage_min_u32(TRIAGE_SHEET_WIDTH, ui->canvas.width > 40U ? ui->canvas.width - 40U : ui->canvas.width);
	uint32_t height = triage_min_u32(TRIAGE_SHEET_HEIGHT, ui->canvas.height > 40U ? ui->canvas.height - 40U : ui->canvas.height);
	int32_t x = (int32_t)((ui->canvas.width - width) / 2U);
	int32_t y = (int32_t)TRIAGE_BAR_HEIGHT + 18;
	sheet->frame = triage_rect(x, y, width, height);

	int32_t column = x + 260;
	uint32_t column_width = width > 284U ? width - 284U : width / 2U;
	int32_t top = y + 88;

	sheet->name_field = triage_rect(column, top + 18, column_width, TRIAGE_SHEET_FIELD_HEIGHT);

	uint32_t segment = column_width / 3U;
	int32_t role_y = top + 80;
	sheet->role_data = triage_rect(column, role_y, segment, 28U);
	sheet->role_system = triage_rect(column + (int32_t)segment, role_y, segment, 28U);
	sheet->role_efi = triage_rect(column + (int32_t)(segment * 2U), role_y, column_width - segment * 2U, 28U);

	uint32_t unit_width = 92U;
	uint32_t size_width = column_width > unit_width + 12U ? column_width - unit_width - 12U : column_width / 2U;
	int32_t size_y = top + 142;
	sheet->size_field = triage_rect(column, size_y, size_width, TRIAGE_SHEET_FIELD_HEIGHT);
	sheet->unit_mib = triage_rect(column + (int32_t)(column_width - unit_width), size_y, unit_width / 2U, TRIAGE_SHEET_FIELD_HEIGHT);
	sheet->unit_gib = triage_rect(sheet->unit_mib.x + (int32_t)(unit_width / 2U), size_y, unit_width - unit_width / 2U, TRIAGE_SHEET_FIELD_HEIGHT);

	sheet->slider = triage_rect(column, top + 190, column_width, 22U);
	sheet->use_all_box = triage_rect(column, top + 226, 18U, 18U);

	uint32_t button_height = 32U;
	int32_t button_y = y + (int32_t)height - (int32_t)TRIAGE_SHEET_PADDING - (int32_t)button_height;
	sheet->apply = triage_rect(x + (int32_t)width - (int32_t)TRIAGE_SHEET_PADDING - 136, button_y, 136U, button_height);
	sheet->cancel = triage_rect(sheet->apply.x - 10 - 92, button_y, 92U, button_height);
}

static triage_sheet_control_t
triage_sheet_hit(triage_ui_t *ui, int32_t x, int32_t y)
{
	const triage_disk_sheet_t *sheet = &ui->disk_sheet;
	if (triage_rect_hit(&sheet->name_field, x, y)) return TRIAGE_SHEET_CONTROL_NAME;
	if (triage_rect_hit(&sheet->size_field, x, y)) return TRIAGE_SHEET_CONTROL_SIZE;
	if (triage_rect_hit(&sheet->unit_mib, x, y)) return TRIAGE_SHEET_CONTROL_UNIT_MIB;
	if (triage_rect_hit(&sheet->unit_gib, x, y)) return TRIAGE_SHEET_CONTROL_UNIT_GIB;
	if (triage_rect_hit(&sheet->role_data, x, y)) return TRIAGE_SHEET_CONTROL_ROLE_DATA;
	if (triage_rect_hit(&sheet->role_system, x, y)) return TRIAGE_SHEET_CONTROL_ROLE_SYSTEM;
	if (triage_rect_hit(&sheet->role_efi, x, y)) return TRIAGE_SHEET_CONTROL_ROLE_EFI;
	if (triage_rect_hit(&sheet->slider, x, y)) return TRIAGE_SHEET_CONTROL_SLIDER;
	if (triage_rect_hit(&sheet->use_all_box, x, y)) return TRIAGE_SHEET_CONTROL_USE_ALL;
	if (triage_rect_hit(&sheet->cancel, x, y)) return TRIAGE_SHEET_CONTROL_CANCEL;
	if (triage_rect_hit(&sheet->apply, x, y)) return TRIAGE_SHEET_CONTROL_APPLY;
	return TRIAGE_SHEET_CONTROL_NONE;
}

/* Track position of the slider knob for the size the fields currently hold. */
static int32_t
triage_sheet_knob_x(const triage_disk_sheet_t *sheet)
{
	uint64_t sectors = triage_sheet_requested_sectors(sheet);
	if (sheet->free_sectors == 0ULL) {
		return sheet->slider.x + 9;
	}

	if (sectors > sheet->free_sectors) {
		sectors = sheet->free_sectors;
	}

	uint32_t travel = sheet->slider.width > 18U ? sheet->slider.width - 18U : 0U;
	uint64_t offset = (sectors * travel) / sheet->free_sectors;
	return sheet->slider.x + 9 + (int32_t)offset;
}

static void
triage_sheet_slide_to(triage_ui_t *ui, int32_t x)
{
	triage_disk_sheet_t *sheet = &ui->disk_sheet;
	if (sheet->free_sectors == 0ULL) {
		return;
	}

	uint32_t travel = sheet->slider.width > 18U ? sheet->slider.width - 18U : 1U;
	int32_t offset = x - (sheet->slider.x + 9);
	if (offset < 0) {
		offset = 0;
	}

	if (offset > (int32_t)travel) {
		offset = (int32_t)travel;
	}

	uint64_t sectors = (sheet->free_sectors * (uint64_t)offset) / travel;
	uint64_t bytes = sectors * sheet->sector_size;
	if (bytes < TRIAGE_SHEET_MINIMUM_BYTES) {
		bytes = TRIAGE_SHEET_MINIMUM_BYTES;
	}

	sheet->use_all = false;
	triage_sheet_set_bytes(sheet, bytes);
	triage_mark_sheet(ui);
}

/*
 * Integer atan, in turns rather than radians.
 *
 * The donut needs an angle per sample and this process has no floating point
 * to spare, so the classic quadratic approximation of atan on [0, 1] is
 * evaluated in Q16 and rescaled by 2*pi.  Its worst case error is under a
 * tenth of a degree, which is well inside one pixel at the radii drawn here.
 */
static uint32_t
triage_atan_turn(uint32_t ratio)
{
	int64_t z = (int64_t)ratio;
	int64_t linear = (51472LL * z) >> 16;
	int64_t coefficient = 16036LL + ((4345LL * z) >> 16);
	int64_t correction = (((z * (z - 65536LL)) >> 16) * coefficient) >> 16;
	int64_t radians = linear - correction;
	if (radians < 0LL) {
		radians = 0LL;
	}

	return (uint32_t)((radians << 16) / 411775LL);
}

/* Angle of (dx, dy) as a fraction of a turn clockwise from twelve o'clock. */
static uint32_t
triage_turn(int32_t dx, int32_t dy)
{
	int32_t x = dx;
	int32_t y = -dy;
	uint32_t ax = (uint32_t)(x < 0 ? -x : x);
	uint32_t ay = (uint32_t)(y < 0 ? -y : y);
	if (ax == 0U && ay == 0U) {
		return 0U;
	}

	uint32_t counter;
	if (ax >= ay) {
		counter = triage_atan_turn((uint32_t)(((uint64_t)ay << 16) / ax));
	} else {
		counter = 16384U - triage_atan_turn((uint32_t)(((uint64_t)ax << 16) / ay));
	}

	if (x < 0 && y >= 0) {
		counter = 32768U - counter;
	} else if (x < 0) {
		counter = 32768U + counter;
	} else if (y < 0) {
		counter = 65536U - counter;
	}

	return (16384U + 65536U - counter) & 0xFFFFU;
}

/*
 * Draw the capacity donut.
 *
 * Every primitive in StartupOptionsUI is hard edged, which a 90 pixel circle cannot
 * afford, so this samples each pixel four times and averages the results.  The
 * samples that miss the ring take the sheet fill, which is what feathers the
 * outer and inner edges into the material behind them.
 */
static void
triage_draw_donut(
	triage_ui_t *ui,
	int32_t center_x,
	int32_t center_y,
	uint32_t outer,
	uint32_t inner,
	const uint32_t *bounds,
	const uint32_t *colors,
	uint32_t count,
	uint32_t background
)
{
	startup_options_ui_canvas_t *canvas = &ui->canvas;
	int64_t outer_squared = (int64_t)(outer * 4U) * (int64_t)(outer * 4U);
	int64_t inner_squared = (int64_t)(inner * 4U) * (int64_t)(inner * 4U);
	int32_t first_x = center_x - (int32_t)outer - 1;
	int32_t first_y = center_y - (int32_t)outer - 1;
	int32_t last_x = center_x + (int32_t)outer + 1;
	int32_t last_y = center_y + (int32_t)outer + 1;

	for (int32_t py = first_y; py <= last_y; py++) {
		if (py < 0 || py >= (int32_t)canvas->height) {
			continue;
		}

		for (int32_t px = first_x; px <= last_x; px++) {
			if (px < 0 || px >= (int32_t)canvas->width) {
				continue;
			}

			uint32_t red = 0U;
			uint32_t green = 0U;
			uint32_t blue = 0U;

			for (uint32_t sample = 0U; sample < 4U; sample++) {
				int32_t sx = (px * 4 + (int32_t)((sample & 1U) == 0U ? 1U : 3U)) - (center_x * 4 + 2);
				int32_t sy = (py * 4 + (int32_t)((sample & 2U) == 0U ? 1U : 3U)) - (center_y * 4 + 2);
				int64_t distance = (int64_t)sx * sx + (int64_t)sy * sy;
				uint32_t color = background;

				if (distance <= outer_squared && distance >= inner_squared) {
					uint32_t turn = triage_turn(sx, sy);
					for (uint32_t slice = 0U; slice < count; slice++) {
						if (turn >= bounds[slice] && turn < bounds[slice + 1U]) {
							color = colors[slice];
							break;
						}
					}
				}

				red += (color >> 16U) & 0xFFU;
				green += (color >> 8U) & 0xFFU;
				blue += color & 0xFFU;
			}

			canvas->pixels[(uint32_t)py * canvas->stride + (uint32_t)px] =
				STARTUP_OPTIONS_UI_ARGB(255U, red / 4U, green / 4U, blue / 4U);
		}
	}
}

static void
triage_draw_sheet_field(
	triage_ui_t *ui,
	const triage_rect_t *rect,
	const char *text,
	bool focused,
	bool enabled
)
{
	uint32_t border = focused ? TRIAGE_ACCENT : TRIAGE_SHEET_FIELD_BORDER;
	startup_options_ui_canvas_stroke_squircle(
		&ui->canvas,
		rect->x,
		rect->y,
		rect->width,
		rect->height,
		TRIAGE_SHEET_CONTROL_RADIUS,
		focused ? 2U : 1U,
		border,
		TRIAGE_SHEET_FIELD
	);

	int32_t text_x = rect->x + 10;
	int32_t text_y = triage_text_y(rect->y, rect->height, 1U);
	startup_options_ui_draw_text(&ui->canvas, text_x, text_y, text, enabled ? TRIAGE_TEXT : TRIAGE_MUTED, 1U);

	if (focused && enabled) {
		int32_t caret = text_x + (int32_t)startup_options_ui_text_width(text, 1U) + 1;
		startup_options_ui_canvas_fill_rect(&ui->canvas, caret, rect->y + 7, 1U, rect->height - 14U, TRIAGE_ACCENT);
	}
}

static void
triage_draw_sheet_segment(
	triage_ui_t *ui,
	const triage_rect_t *rect,
	const char *title,
	bool selected,
	bool hovered,
	bool first,
	bool last
)
{
	uint32_t fill = selected
		? TRIAGE_SHEET_SEGMENT_ON
		: (hovered ? TRIAGE_SHEET_SEGMENT_HOVER : TRIAGE_SHEET_SEGMENT);

	/*
	 * Interior segments keep square edges so the group reads as one control:
	 * the capsule is drawn wider than the segment and clipped by its
	 * neighbours, which is cheaper than a per-corner radius.
	 */
	uint32_t radius = TRIAGE_SHEET_CONTROL_RADIUS;
	int32_t x = rect->x - (first ? 0 : (int32_t)radius);
	uint32_t width = rect->width + (first ? 0U : radius) + (last ? 0U : radius);
	startup_options_ui_canvas_fill_squircle(&ui->canvas, x, rect->y, width, rect->height, radius, fill);

	if (!last) {
		startup_options_ui_canvas_fill_rect(&ui->canvas, rect->x + (int32_t)rect->width - 1, rect->y + 5, 1U, rect->height - 10U, TRIAGE_SHEET_BORDER);
	}

	triage_center_text(
		&ui->canvas,
		rect->x + (int32_t)(rect->width / 2U),
		triage_text_y(rect->y, rect->height, 1U),
		title,
		selected ? TRIAGE_TEXT : TRIAGE_MUTED,
		1U
	);
}

static void
triage_draw_sheet_button(
	triage_ui_t *ui,
	const triage_rect_t *rect,
	const char *title,
	bool accent,
	bool hovered,
	bool pressed,
	bool enabled
)
{
	uint32_t fill;
	if (!enabled) {
		fill = accent ? TRIAGE_SHEET_ACCENT_OFF : TRIAGE_SHEET_SEGMENT;
	} else if (accent) {
		fill = pressed ? TRIAGE_ACCENT_PRESSED : (hovered ? TRIAGE_ACCENT_HOVER : TRIAGE_ACCENT);
	} else {
		fill = pressed ? TRIAGE_ROW_PRESSED : (hovered ? TRIAGE_SHEET_SEGMENT_HOVER : TRIAGE_SHEET_SEGMENT);
	}

	startup_options_ui_canvas_fill_squircle(&ui->canvas, rect->x, rect->y, rect->width, rect->height, TRIAGE_SHEET_BUTTON_RADIUS, fill);
	triage_center_text(
		&ui->canvas,
		rect->x + (int32_t)(rect->width / 2U),
		triage_text_y(rect->y, rect->height, 1U),
		title,
		enabled ? TRIAGE_TEXT : TRIAGE_MUTED,
		1U
	);
}

/*
 * Dim and shadow: everything outside the sheet's own opaque rectangle.
 *
 * Drawn only when the frame behind the sheet is being repainted anyway, since
 * neither depends on any control's state.
 */
static void
triage_draw_sheet_backdrop(triage_ui_t *ui)
{
	const triage_disk_sheet_t *sheet = &ui->disk_sheet;
	startup_options_ui_canvas_fill_rect(&ui->canvas, 0, 0, ui->canvas.width, ui->canvas.height, TRIAGE_MODAL_DIM);
	startup_options_ui_canvas_shadow_squircle(
		&ui->canvas,
		sheet->frame.x,
		sheet->frame.y,
		sheet->frame.width,
		sheet->frame.height,
		TRIAGE_SHEET_RADIUS,
		26U,
		12,
		150U
	);
}

static void
triage_draw_sheet_body(triage_ui_t *ui, bool redraw_frame)
{
	triage_disk_sheet_t *sheet = &ui->disk_sheet;
	triage_sheet_layout(ui);

	int32_t x = sheet->frame.x;
	int32_t y = sheet->frame.y;
	uint32_t width = sheet->frame.width;
	uint32_t height = sheet->frame.height;

	if (redraw_frame) {
		startup_options_ui_canvas_stroke_squircle(&ui->canvas, x, y, width, height, TRIAGE_SHEET_RADIUS, 1U, TRIAGE_SHEET_BORDER, TRIAGE_SHEET);
	} else {
		uint32_t inset = TRIAGE_SHEET_RADIUS;
		startup_options_ui_canvas_fill_rect(&ui->canvas, x + (int32_t)inset, y + 1, width - inset * 2U, height - 2U, TRIAGE_SHEET);
		startup_options_ui_canvas_fill_rect(&ui->canvas, x + 1, y + (int32_t)inset, width - 2U, height - inset * 2U, TRIAGE_SHEET);
	}

	int32_t left = x + (int32_t)TRIAGE_SHEET_PADDING;
	startup_options_ui_draw_semibold_text(&ui->canvas, left, y + 20, "Add a Partition", TRIAGE_TEXT, 2U);

	char subtitle[96];
	char free_text[32];
	triage_format_size(free_text, sizeof(free_text), sheet->free_sectors * sheet->sector_size);
	subtitle[0] = '\0';
	triage_append(subtitle, sizeof(subtitle), ui->disk_info.name[0] != '\0' ? ui->disk_info.name : "disk0");
	triage_append(subtitle, sizeof(subtitle), "  |  ");
	triage_append(subtitle, sizeof(subtitle), triage_layout_name(ui->disk_layout.scheme));
	triage_append(subtitle, sizeof(subtitle), "  |  ");
	triage_append(subtitle, sizeof(subtitle), free_text);
	triage_append(subtitle, sizeof(subtitle), " free");
	startup_options_ui_draw_text(&ui->canvas, left, y + 46, subtitle, TRIAGE_MUTED, 1U);
	startup_options_ui_canvas_fill_rect(&ui->canvas, x + 1, y + 72, width - 2U, 1U, TRIAGE_SHEET_BORDER);

	/*
	 * Capacity donut: the existing partitions in their map order, then the
	 * proposed one in the accent colour, then whatever free space is left.
	 */
	uint32_t bounds[TRIAGE_DISK_PARTITIONS_MAX + 3U];
	uint32_t colors[TRIAGE_DISK_PARTITIONS_MAX + 2U];
	/* The accent is reserved for the proposed partition, so existing ones never use it. */
	static const uint32_t palette[4] = { TRIAGE_DISK_ALT1, TRIAGE_DISK_ALT2, TRIAGE_DISK_ALT3, TRIAGE_DISK_EXISTING };
	uint32_t slices = 0U;
	uint64_t total = sheet->disk_sectors != 0ULL ? sheet->disk_sectors : 1ULL;
	uint64_t consumed = 0ULL;

	bounds[0] = 0U;
	for (uint32_t index = 0U; index < ui->disk_partition_count && slices < TRIAGE_DISK_PARTITIONS_MAX; index++) {
		consumed += ui->disk_partitions[index].sector_count;
		colors[slices] = palette[index % 4U];
		slices++;
		bounds[slices] = (uint32_t)((consumed * 65536ULL) / total);
	}

	uint64_t proposed = triage_sheet_requested_sectors(sheet);
	if (proposed > sheet->free_sectors) {
		proposed = sheet->free_sectors;
	}

	if (proposed != 0ULL) {
		consumed += proposed;
		colors[slices] = TRIAGE_ACCENT;
		slices++;
		bounds[slices] = (uint32_t)((consumed * 65536ULL) / total);
	}

	colors[slices] = TRIAGE_DISK_FREE;
	slices++;
	bounds[slices] = 65536U;

	int32_t center_x = x + 132;
	int32_t center_y = y + 196;
	triage_draw_donut(ui, center_x, center_y, 92U, 58U, bounds, colors, slices, TRIAGE_SHEET);

	char donut_size[32];
	triage_format_size(donut_size, sizeof(donut_size), proposed * sheet->sector_size);
	triage_center_text(&ui->canvas, center_x, center_y - 20, donut_size, TRIAGE_TEXT, 2U);
	char donut_caption[48] = "New ";
	triage_append(donut_caption, sizeof(donut_caption), triage_sheet_role_name(sheet->role));
	triage_center_text(&ui->canvas, center_x, center_y + 4, donut_caption, TRIAGE_MUTED, 1U);

	int32_t column = sheet->name_field.x;
	bool named = ui->disk_layout.scheme != RECOVERY_BLOCK_LAYOUT_MBR;

	startup_options_ui_draw_text(&ui->canvas, column, sheet->name_field.y - 20, "Name", TRIAGE_MUTED, 1U);
	triage_draw_sheet_field(
		ui,
		&sheet->name_field,
		named ? sheet->name : "Unnamed on MBR",
		named && sheet->focus == TRIAGE_SHEET_CONTROL_NAME,
		named
	);

	startup_options_ui_draw_text(&ui->canvas, column, sheet->role_data.y - 20, "Format", TRIAGE_MUTED, 1U);
	triage_draw_sheet_segment(
		ui,
		&sheet->role_data,
		"Data",
		sheet->role == (uint32_t)RECOVERY_BLOCK_PARTITION_ROLE_DATA,
		sheet->hovered == TRIAGE_SHEET_CONTROL_ROLE_DATA,
		true,
		false
	);
	triage_draw_sheet_segment(
		ui,
		&sheet->role_system,
		"System",
		sheet->role == (uint32_t)RECOVERY_BLOCK_PARTITION_ROLE_SYSTEM,
		sheet->hovered == TRIAGE_SHEET_CONTROL_ROLE_SYSTEM,
		false,
		false
	);
	triage_draw_sheet_segment(
		ui,
		&sheet->role_efi,
		"EFI",
		sheet->role == (uint32_t)RECOVERY_BLOCK_PARTITION_ROLE_EFI,
		sheet->hovered == TRIAGE_SHEET_CONTROL_ROLE_EFI,
		false,
		true
	);

	startup_options_ui_draw_text(&ui->canvas, column, sheet->size_field.y - 20, "Size", TRIAGE_MUTED, 1U);
	char size_display[TRIAGE_SHEET_SIZE_MAX + 8U];
	if (sheet->use_all) {
		uint64_t unit = triage_sheet_unit_bytes(sheet);
		triage_u64(size_display, sizeof(size_display), (sheet->free_sectors * sheet->sector_size) / unit);
	} else {
		triage_copy(size_display, sizeof(size_display), sheet->size_text);
	}

	triage_draw_sheet_field(
		ui,
		&sheet->size_field,
		size_display,
		!sheet->use_all && sheet->focus == TRIAGE_SHEET_CONTROL_SIZE,
		!sheet->use_all
	);
	triage_draw_sheet_segment(ui, &sheet->unit_mib, "MiB", !sheet->gibibytes, sheet->hovered == TRIAGE_SHEET_CONTROL_UNIT_MIB, true, false);
	triage_draw_sheet_segment(ui, &sheet->unit_gib, "GiB", sheet->gibibytes, sheet->hovered == TRIAGE_SHEET_CONTROL_UNIT_GIB, false, true);

	int32_t track_y = sheet->slider.y + (int32_t)(sheet->slider.height / 2U) - 2;
	startup_options_ui_canvas_fill_squircle(&ui->canvas, sheet->slider.x, track_y, sheet->slider.width, 4U, 2U, TRIAGE_SHEET_TRACK);
	int32_t knob_x = triage_sheet_knob_x(sheet);
	uint32_t filled = (uint32_t)(knob_x - sheet->slider.x);
	if (filled != 0U) {
		startup_options_ui_canvas_fill_squircle(&ui->canvas, sheet->slider.x, track_y, filled, 4U, 2U, TRIAGE_ACCENT);
	}

	startup_options_ui_canvas_fill_circle(&ui->canvas, knob_x, sheet->slider.y + (int32_t)(sheet->slider.height / 2U), 9U, TRIAGE_SHEET_KNOB);

	startup_options_ui_canvas_stroke_squircle(
		&ui->canvas,
		sheet->use_all_box.x,
		sheet->use_all_box.y,
		sheet->use_all_box.width,
		sheet->use_all_box.height,
		7U,
		1U,
		sheet->use_all ? TRIAGE_ACCENT : TRIAGE_SHEET_FIELD_BORDER,
		sheet->use_all ? TRIAGE_ACCENT : TRIAGE_SHEET_FIELD
	);
	if (sheet->use_all) {
		/* Checkmark: two strokes, drawn as stepped pixels so it stays crisp. */
		for (uint32_t step = 0U; step < 4U; step++) {
			startup_options_ui_canvas_fill_rect(&ui->canvas, sheet->use_all_box.x + 4 + (int32_t)step, sheet->use_all_box.y + 8 + (int32_t)step, 2U, 2U, TRIAGE_TEXT);
		}

		for (uint32_t step = 0U; step < 6U; step++) {
			startup_options_ui_canvas_fill_rect(&ui->canvas, sheet->use_all_box.x + 7 + (int32_t)step, sheet->use_all_box.y + 11 - (int32_t)step, 2U, 2U, TRIAGE_TEXT);
		}
	}

	startup_options_ui_draw_text(
		&ui->canvas,
		sheet->use_all_box.x + (int32_t)sheet->use_all_box.width + 9,
		triage_text_y(sheet->use_all_box.y, sheet->use_all_box.height, 1U),
		"Use all available space",
		TRIAGE_TEXT,
		1U
	);

	/*
	 * The footer explains why Add Partition is dimmed.  A size the disk
	 * cannot satisfy is the common case, so it displaces the stored message
	 * rather than waiting for the button to be pressed.
	 */
	char hint[TRIAGE_SHEET_MESSAGE_MAX];
	const char *message = sheet->message;
	if (sheet->free_sectors != 0ULL && !sheet->use_all && !triage_sheet_size_valid(sheet)) {
		hint[0] = '\0';
		triage_append(hint, sizeof(hint), "Enter a size from 1 MiB to ");
		triage_append(hint, sizeof(hint), free_text);
		triage_append(hint, sizeof(hint), ".");
		message = hint;
	}

	if (message[0] != '\0') {
		startup_options_ui_draw_text(&ui->canvas, left, triage_text_y(sheet->cancel.y, sheet->cancel.height, 1U), message, TRIAGE_MUTED, 1U);
	}

	bool can_apply = sheet->free_sectors != 0ULL && (sheet->use_all || triage_sheet_size_valid(sheet));
	triage_draw_sheet_button(
		ui,
		&sheet->cancel,
		"Cancel",
		false,
		sheet->hovered == TRIAGE_SHEET_CONTROL_CANCEL,
		sheet->pressed == TRIAGE_SHEET_CONTROL_CANCEL,
		true
	);
	triage_draw_sheet_button(
		ui,
		&sheet->apply,
		"Add Partition",
		true,
		sheet->hovered == TRIAGE_SHEET_CONTROL_APPLY,
		sheet->pressed == TRIAGE_SHEET_CONTROL_APPLY,
		can_apply
	);
}

static void
triage_draw_sheet(triage_ui_t *ui)
{
	triage_sheet_layout(ui);
	triage_draw_sheet_backdrop(ui);
	triage_draw_sheet_body(ui, true);
}

static bool
triage_sheet_pointer_move(triage_ui_t *ui, int32_t x, int32_t y)
{
	triage_disk_sheet_t *sheet = &ui->disk_sheet;
	if (sheet->dragging) {
		triage_sheet_slide_to(ui, x);
		return true;
	}

	triage_sheet_layout(ui);
	triage_sheet_control_t hovered = triage_sheet_hit(ui, x, y);
	if (hovered == sheet->hovered) {
		return false;
	}

	sheet->hovered = hovered;
	triage_mark_sheet(ui);
	return true;
}

static bool
triage_sheet_pointer_down(triage_ui_t *ui, int32_t x, int32_t y)
{
	triage_disk_sheet_t *sheet = &ui->disk_sheet;
	triage_sheet_layout(ui);
	triage_sheet_control_t control = triage_sheet_hit(ui, x, y);
	sheet->pressed = control;

	if (control == TRIAGE_SHEET_CONTROL_SLIDER) {
		sheet->dragging = true;
		triage_sheet_slide_to(ui, x);
		return true;
	}

	if (control == TRIAGE_SHEET_CONTROL_NAME || control == TRIAGE_SHEET_CONTROL_SIZE) {
		sheet->focus = control;
	}

	triage_mark_sheet(ui);
	return true;
}

static bool
triage_sheet_pointer_up(triage_ui_t *ui, int32_t x, int32_t y)
{
	triage_disk_sheet_t *sheet = &ui->disk_sheet;
	triage_sheet_layout(ui);

	if (sheet->dragging) {
		sheet->dragging = false;
		sheet->pressed = TRIAGE_SHEET_CONTROL_NONE;
		triage_mark_sheet(ui);
		return true;
	}

	triage_sheet_control_t control = triage_sheet_hit(ui, x, y);
	bool activated = control != TRIAGE_SHEET_CONTROL_NONE && control == sheet->pressed;
	sheet->pressed = TRIAGE_SHEET_CONTROL_NONE;

	if (!activated) {
		triage_mark_sheet(ui);
		return true;
	}

	uint64_t bytes = triage_sheet_requested_bytes(sheet);

	switch (control) {
	case TRIAGE_SHEET_CONTROL_UNIT_MIB:
	case TRIAGE_SHEET_CONTROL_UNIT_GIB:
		sheet->gibibytes = control == TRIAGE_SHEET_CONTROL_UNIT_GIB;
		triage_sheet_set_bytes(sheet, bytes);
		break;
	case TRIAGE_SHEET_CONTROL_ROLE_DATA:
		sheet->role = (uint32_t)RECOVERY_BLOCK_PARTITION_ROLE_DATA;
		break;
	case TRIAGE_SHEET_CONTROL_ROLE_SYSTEM:
		sheet->role = (uint32_t)RECOVERY_BLOCK_PARTITION_ROLE_SYSTEM;
		break;
	case TRIAGE_SHEET_CONTROL_ROLE_EFI:
		sheet->role = (uint32_t)RECOVERY_BLOCK_PARTITION_ROLE_EFI;
		break;
	case TRIAGE_SHEET_CONTROL_USE_ALL:
		sheet->use_all = !sheet->use_all;
		if (!sheet->use_all) {
			triage_sheet_set_bytes(sheet, sheet->free_sectors * sheet->sector_size);
		}

		break;
	case TRIAGE_SHEET_CONTROL_CANCEL:
		triage_sheet_close(ui);
		triage_disk_set_status(ui, "Add partition cancelled.");
		return true;
	case TRIAGE_SHEET_CONTROL_APPLY:
		triage_sheet_apply(ui);
		return true;
	default:
		break;
	}

	triage_mark_sheet(ui);
	return true;
}

static bool
triage_sheet_key(triage_ui_t *ui, uint32_t code, uint32_t modifiers)
{
	triage_disk_sheet_t *sheet = &ui->disk_sheet;
	triage_sheet_layout(ui);

	if (code == TRIAGE_KEY_ESCAPE) {
		triage_sheet_close(ui);
		triage_disk_set_status(ui, "Add partition cancelled.");
		return true;
	}

	if (code == TRIAGE_KEY_ENTER) {
		triage_sheet_apply(ui);
		return true;
	}

	if (code == TRIAGE_KEY_TAB) {
		sheet->focus = sheet->focus == TRIAGE_SHEET_CONTROL_NAME
			? TRIAGE_SHEET_CONTROL_SIZE
			: TRIAGE_SHEET_CONTROL_NAME;
		triage_mark_sheet(ui);
		return true;
	}

	/* Arrow keys nudge the size by one unit, the way a stepper would. */
	if ((code == TRIAGE_KEY_UP || code == TRIAGE_KEY_DOWN) && !sheet->use_all) {
		uint64_t unit = triage_sheet_unit_bytes(sheet);
		uint64_t bytes = triage_sheet_requested_bytes(sheet);
		if (code == TRIAGE_KEY_UP) {
			bytes += unit;
		} else {
			bytes = bytes > unit ? bytes - unit : 0ULL;
		}

		uint64_t maximum = sheet->free_sectors * sheet->sector_size;
		if (bytes > maximum) {
			bytes = maximum;
		}

		triage_sheet_set_bytes(sheet, bytes);
		triage_mark_sheet(ui);
		return true;
	}

	if (code == TRIAGE_KEY_BACKSPACE) {
		if (sheet->focus == TRIAGE_SHEET_CONTROL_SIZE && sheet->size_length != 0U && !sheet->use_all) {
			sheet->size_text[--sheet->size_length] = '\0';
			triage_mark_sheet(ui);
			return true;
		}

		if (sheet->focus == TRIAGE_SHEET_CONTROL_NAME && sheet->name_length != 0U) {
			sheet->name[--sheet->name_length] = '\0';
			triage_mark_sheet(ui);
			return true;
		}

		return false;
	}

	char character = triage_key_character(code, modifiers);
	if (character == '\0') {
		return false;
	}

	if (sheet->focus == TRIAGE_SHEET_CONTROL_SIZE) {
		if (sheet->use_all || character < '0' || character > '9') {
			return false;
		}

		if (sheet->size_length + 1U >= sizeof(sheet->size_text)) {
			return false;
		}

		sheet->size_text[sheet->size_length++] = character;
		sheet->size_text[sheet->size_length] = '\0';
		triage_mark_sheet(ui);
		return true;
	}

	if (sheet->focus == TRIAGE_SHEET_CONTROL_NAME) {
		if (ui->disk_layout.scheme == RECOVERY_BLOCK_LAYOUT_MBR) {
			return false;
		}

		if (sheet->name_length + 1U >= sizeof(sheet->name)) {
			return false;
		}

		sheet->name[sheet->name_length++] = character;
		sheet->name[sheet->name_length] = '\0';
		triage_mark_sheet(ui);
		return true;
	}

	return false;
}

static void
triage_disk_add(triage_ui_t *ui)
{
	ui->disk_delete_armed = false;

	if (ui->disk_mounted) {
		ui->disk_initialize_armed = false;
		triage_disk_set_status(ui, "Unmount the disk before changing its partition map.");
		triage_mark_disk(ui);
		return;
	}

	if (ui->disk_layout.scheme == RECOVERY_BLOCK_LAYOUT_RAW) {
		ui->disk_initialize_armed = false;
		triage_disk_set_status(ui, "Partition-map editing is not available yet; inspection and verification are safe to use.");
		triage_mark_disk(ui);
		return;
	}

	triage_disk_set_status(ui, "");
	triage_sheet_open(ui);
}

static void
triage_disk_delete(triage_ui_t *ui)
{
	ui->disk_initialize_armed = false;

	if (ui->disk_partition_count == 0U) {
		ui->disk_delete_armed = false;
		triage_disk_set_status(ui, "There is no partition to delete.");
		triage_mark_disk(ui);
		return;
	}

	if (ui->disk_mounted) {
		ui->disk_delete_armed = false;
		triage_disk_set_status(ui, "Unmount sevOS System before deleting a partition.");
		triage_mark_disk(ui);
		return;
	}

	if (triage_equal(ui->disk_partitions[ui->disk_selected_partition].name, "sevOS Recovery")) {
		ui->disk_delete_armed = false;
		triage_disk_set_status(ui, "sevOS Recovery is in use and cannot delete itself.");
		triage_mark_disk(ui);
		return;
	}

	if (!ui->disk_delete_armed) {
		ui->disk_delete_armed = true;
		triage_disk_set_status(ui, "Deleting a partition loses its data. Click Delete again to confirm.");
		triage_mark_disk(ui);
		return;
	}

	ui->disk_delete_armed = false;
	int64_t result = recovery_block_partition_delete(0U, ui->disk_selected_partition);
	triage_disk_refresh(ui);

	if (result < 0) {
		triage_disk_set_error(ui, "Delete partition failed", result);
	} else {
		triage_disk_set_status(ui, "Partition deleted. Restart recovery to refresh block devices.");
	}

	triage_mark_disk(ui);
}

static void
triage_disk_enter(triage_ui_t *ui)
{
	ui->disk_active = true;
	ui->terminal_active = false;
	ui->disk_sheet.active = false;
	ui->disk_selected_partition = 0U;
	ui->disk_initialize_armed = false;
	ui->disk_delete_armed = false;
	ui->disk_status[0] = '\0';
	for (uint32_t index = 0U; index < (uint32_t)TRIAGE_DISK_BUTTON_COUNT; index++) {
		ui->disk_button_hovered[index] = false;
		ui->disk_button_pressed[index] = false;
	}

	triage_disk_refresh(ui);
	/* Hit testing runs before the first draw, so place the buttons now. */
	triage_disk_layout(ui);
	/* The title bar changes to "Disk Utility" as well, so this is a full pass. */
	triage_mark_full(ui);
}

/*
 * Two of the five buttons rename themselves after what the disk is currently
 * doing, so the label is derived rather than stored: the incremental path
 * redraws a button without the surrounding panel and has to reach the same
 * answer the full pass would.
 */
static const char *
triage_disk_button_title(const triage_ui_t *ui, uint32_t index)
{
	switch (index) {
	case TRIAGE_DISK_BUTTON_REFRESH: return "Refresh";
	case TRIAGE_DISK_BUTTON_VERIFY: return "Verify";
	case TRIAGE_DISK_BUTTON_DELETE: return "Delete";
	case TRIAGE_DISK_BUTTON_ADD: return ui->disk_layout.scheme == RECOVERY_BLOCK_LAYOUT_RAW ? "Initialize" : "Add";
	default: return ui->disk_mounted ? "Unmount" : "Mount";
	}
}

/* Mount is the panel's default action; Initialize borrows the accent once armed. */
static bool
triage_disk_button_accent(const triage_ui_t *ui, uint32_t index)
{
	if (index == (uint32_t)TRIAGE_DISK_BUTTON_ADD) {
		return ui->disk_initialize_armed;
	}

	return index == (uint32_t)TRIAGE_DISK_BUTTON_PRIMARY;
}

static void
triage_draw_disk_button(triage_ui_t *ui, uint32_t index)
{
	const triage_rect_t *rect = &ui->disk_buttons[index];
	bool accent = triage_disk_button_accent(ui, index);
	uint32_t normal = accent ? TRIAGE_ACCENT : TRIAGE_ROW;
	uint32_t hover = accent ? TRIAGE_ACCENT_HOVER : TRIAGE_ROW_HOVER;
	uint32_t down = accent ? TRIAGE_ACCENT_PRESSED : TRIAGE_ROW_PRESSED;
	uint32_t fill = ui->disk_button_pressed[index] ? down : (ui->disk_button_hovered[index] ? hover : normal);

	/*
	 * Lay the panel back down first.  The squircle antialiases its corners
	 * against whatever is already on the canvas, so a button repainted in
	 * place would blend into its own previous fill instead of the panel and
	 * darken a little on every hover.
	 */
	startup_options_ui_canvas_fill_rect(&ui->canvas, rect->x, rect->y, rect->width, rect->height, TRIAGE_PANEL);
	startup_options_ui_canvas_fill_squircle(&ui->canvas, rect->x, rect->y, rect->width, rect->height, TRIAGE_DISK_BUTTON_RADIUS, fill);
	triage_center_text(
		&ui->canvas,
		rect->x + (int32_t)(rect->width / 2U),
		triage_text_y(rect->y, rect->height, 1U),
		triage_disk_button_title(ui, index),
		TRIAGE_TEXT,
		1U
	);
}

static int32_t
triage_disk_button_at(const triage_ui_t *ui, int32_t x, int32_t y)
{
	for (uint32_t index = 0U; index < (uint32_t)TRIAGE_DISK_BUTTON_COUNT; index++) {
		const triage_rect_t *rect = &ui->disk_buttons[index];
		if (startup_options_ui_point_in_squircle(x, y, rect->x, rect->y, rect->width, rect->height, TRIAGE_DISK_BUTTON_RADIUS)) {
			return (int32_t)index;
		}
	}

	return -1;
}

static void
triage_draw_disk_utility(triage_ui_t *ui)
{
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	triage_workspace(ui, &x, &y, &width, &height);
	triage_disk_layout(ui);

	if (!ui->disk_sheet.active) triage_restore_background(ui, x, y, width, height);
	startup_options_ui_canvas_stroke_squircle(&ui->canvas, x, y, width, height, 14U, 1U, TRIAGE_BORDER, TRIAGE_PANEL);
	startup_options_ui_draw_text(&ui->canvas, x + 28, y + 22, "Disk Utility", TRIAGE_TEXT, 3U);
	startup_options_ui_draw_text(&ui->canvas, x + 28, y + 58, "Esc returns  |  Select a partition before deleting", TRIAGE_MUTED, 1U);

	char capacity[32] = "Unavailable";
	if (ui->disk_info.sector_count != 0ULL && ui->disk_info.sector_size != 0U) {
		triage_format_bytes(capacity, sizeof(capacity), ui->disk_info.sector_count * (uint64_t)ui->disk_info.sector_size);
	}

	startup_options_ui_draw_text(&ui->canvas, x + 28, y + 102, ui->disk_info.name[0] != '\0' ? ui->disk_info.name : "disk0", TRIAGE_TEXT, 2U);
	startup_options_ui_draw_text(&ui->canvas, x + 28, y + 130, capacity, TRIAGE_MUTED, 1U);

	char layout[64] = "Partition map: ";
	triage_append(layout, sizeof(layout), triage_layout_name(ui->disk_layout.scheme));
	startup_options_ui_draw_text(&ui->canvas, x + 28, y + 156, layout, TRIAGE_MUTED, 1U);

	char health_line[96] = "Health: unavailable";
	if (ui->disk_health_valid) {
		health_line[0] = '\0';
		triage_append(health_line, sizeof(health_line), "Health: ");
		triage_append(health_line, sizeof(health_line), ui->disk_health.healthy != 0U ? "Healthy" : "Attention required");

		if (ui->disk_health.read_only != 0U) {
			triage_append(health_line, sizeof(health_line), "  |  Read only");
		}

		triage_append(health_line, sizeof(health_line), ui->disk_health.flush_supported != 0U ? "  |  Flush supported" : "  |  No flush feature");
	}

	startup_options_ui_draw_text(&ui->canvas, x + 28, y + 180, health_line, ui->disk_health_valid && ui->disk_health.healthy != 0U ? TRIAGE_TEXT : TRIAGE_MUTED, 1U);

	if (ui->disk_health_valid) {
		uint64_t errors = ui->disk_health.read_errors + ui->disk_health.write_errors + ui->disk_health.flush_errors;
		char errors_text[24];
		triage_u64(errors_text, sizeof(errors_text), errors);
		char io_line[96] = "I/O errors observed: ";
		triage_append(io_line, sizeof(io_line), errors_text);
		startup_options_ui_draw_text(&ui->canvas, x + 28, y + 202, io_line, TRIAGE_MUTED, 1U);
	}

	int32_t bar_x = x + 28;
	int32_t bar_y = y + 226;
	uint32_t bar_width = width - 56U;
	uint32_t bar_height = 36U;
	startup_options_ui_canvas_fill_squircle(&ui->canvas, bar_x, bar_y, bar_width, bar_height, 8U, TRIAGE_DISK_FREE);

	if (ui->disk_partition_count != 0U && ui->disk_info.sector_count != 0ULL) {
		static const uint32_t colors[4] = { TRIAGE_DISK_USED, TRIAGE_DISK_ALT1, TRIAGE_DISK_ALT2, TRIAGE_DISK_ALT3 };

		for (uint32_t index = 0U; index < ui->disk_partition_count; index++) {
			const recovery_block_partition_info_t *partition = &ui->disk_partitions[index];
			if (partition->start_sector >= ui->disk_info.sector_count || partition->sector_count == 0ULL) continue;

			uint64_t available = ui->disk_info.sector_count - partition->start_sector;
			uint64_t sectors = partition->sector_count < available ? partition->sector_count : available;
			uint32_t segment_x = (uint32_t)((partition->start_sector * bar_width) / ui->disk_info.sector_count);
			uint32_t segment = (uint32_t)((sectors * bar_width) / ui->disk_info.sector_count);
			if (segment == 0U) segment = 1U;
			if (segment_x >= bar_width) continue;
			if (segment > bar_width - segment_x) segment = bar_width - segment_x;

			startup_options_ui_canvas_fill_rect(&ui->canvas, bar_x + (int32_t)segment_x, bar_y, segment, bar_height, colors[index % 4U]);
		}
	} else if (ui->disk_space_valid && ui->disk_space.total_bytes != 0ULL) {
		uint64_t used = ui->disk_space.total_bytes > ui->disk_space.free_bytes
			? ui->disk_space.total_bytes - ui->disk_space.free_bytes
			: 0ULL;
		uint32_t used_width = (uint32_t)((used * bar_width) / ui->disk_space.total_bytes);
		if (used_width > bar_width) used_width = bar_width;

		if (used_width != 0U) {
			startup_options_ui_canvas_fill_squircle(&ui->canvas, bar_x, bar_y, used_width, bar_height, 8U, TRIAGE_DISK_USED);
		}
	}

	int32_t detail_y = y + 286;
	if (ui->disk_partition_count == 0U) {
		startup_options_ui_draw_text(&ui->canvas, x + 28, detail_y, ui->disk_layout.scheme == RECOVERY_BLOCK_LAYOUT_RAW ? "sevOS Volume" : "No partitions", TRIAGE_TEXT, 2U);

		if (ui->disk_layout.scheme == RECOVERY_BLOCK_LAYOUT_RAW) {
			startup_options_ui_draw_text(&ui->canvas, x + 28, detail_y + 28, "Raw ext4 occupies the whole block device. Initialize GPT is destructive.", TRIAGE_MUTED, 1U);
		} else {
			startup_options_ui_draw_text(&ui->canvas, x + 28, detail_y + 28, "The partition map is empty. Add creates a partition in the largest free region.", TRIAGE_MUTED, 1U);
		}

		if (ui->disk_space_valid) {
			char used_line[96] = "Used ";
			char free_line[32];
			char used_bytes[32];
			uint64_t used = ui->disk_space.total_bytes > ui->disk_space.free_bytes
				? ui->disk_space.total_bytes - ui->disk_space.free_bytes
				: 0ULL;
			triage_format_bytes(free_line, sizeof(free_line), ui->disk_space.free_bytes);
			triage_format_bytes(used_bytes, sizeof(used_bytes), used);
			triage_append(used_line, sizeof(used_line), used_bytes);
			triage_append(used_line, sizeof(used_line), "  |  Free ");
			triage_append(used_line, sizeof(used_line), free_line);
			startup_options_ui_draw_text(&ui->canvas, x + 28, detail_y + 54, used_line, TRIAGE_MUTED, 1U);
		}

		char mount_line[96] = "Filesystem ext4  |  ";
		if (ui->disk_mounted) {
			triage_append(mount_line, sizeof(mount_line), "Mounted at ");
			triage_append(mount_line, sizeof(mount_line), ui->disk_mount_path);
		} else {
			triage_append(mount_line, sizeof(mount_line), "Not mounted");
		}

		startup_options_ui_draw_text(&ui->canvas, x + 28, detail_y + 80, mount_line, TRIAGE_MUTED, 1U);
	} else {
		for (uint32_t index = 0U; index < ui->disk_partition_count; index++) {
			const recovery_block_partition_info_t *partition = &ui->disk_partitions[index];
			int32_t row_y = detail_y + (int32_t)(index * 34U) - 5;

			if (index == ui->disk_selected_partition) {
				startup_options_ui_canvas_fill_squircle(&ui->canvas, x + 22, row_y, width - 44U, 30U, 7U, TRIAGE_ROW_SELECTED);
			}

			char size[32];
			triage_format_bytes(size, sizeof(size), partition->sector_count * (uint64_t)ui->disk_info.sector_size);
			char line[96];
			line[0] = '\0';
			triage_append(line, sizeof(line), partition->name[0] != '\0' ? partition->name : "Partition");
			triage_append(line, sizeof(line), "  ");
			triage_append(line, sizeof(line), size);
			startup_options_ui_draw_text(&ui->canvas, x + 28, detail_y + (int32_t)(index * 34U), line, TRIAGE_TEXT, 2U);
		}
	}

	for (uint32_t index = 0U; index < (uint32_t)TRIAGE_DISK_BUTTON_COUNT; index++) {
		triage_draw_disk_button(ui, index);
	}

	if (ui->disk_status[0] != '\0') {
		const triage_rect_t *primary = &ui->disk_buttons[TRIAGE_DISK_BUTTON_PRIMARY];
		startup_options_ui_draw_text(&ui->canvas, x + 28, triage_text_y(primary->y, primary->height, 1U), ui->disk_status, TRIAGE_MUTED, 1U);
	}

	if (ui->disk_sheet.active) {
		triage_draw_sheet(ui);
	}
}

static bool
triage_activate_selected(triage_ui_t *ui)
{
	if (ui->selected_button < 0 || ui->selected_button >= 4) {
		return false;
	}

	triage_action_t action = ui->buttons[(uint32_t)ui->selected_button].action;
	if (action == TRIAGE_ACTION_TERMINAL) {
		triage_terminal_enter(ui);
		return true;
	}

	if (action == TRIAGE_ACTION_DISK_UTILITY) {
		triage_disk_enter(ui);
		return true;
	}

	if (action == TRIAGE_ACTION_REINSTALL || action == TRIAGE_ACTION_RESTART) {
		ui->pending_action = action;
		return true;
	}

	ui->modal_custom = false;
	ui->modal_action = action;
	ui->modal_button_hovered = false;
	ui->modal_button_pressed = false;
	triage_mark_full(ui);
	return true;
}

bool
triage_ui_init(triage_ui_t *ui, uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride)
{
	if (ui == 0 || !startup_options_ui_canvas_init(&ui->canvas, pixels, width, height, stride)) {
		return false;
	}

	ui->selected_button = 0;
	ui->hovered_button = -1;
	ui->pressed_button = -1;
	ui->continue_button_hovered = false;
	ui->continue_button_pressed = false;
	ui->modal_action = TRIAGE_ACTION_NONE;
	ui->pending_action = TRIAGE_ACTION_NONE;
	ui->modal_button_hovered = false;
	ui->modal_button_pressed = false;
	ui->modal_custom = false;
	ui->modal_title[0] = '\0';
	ui->modal_message[0] = '\0';
	ui->terminal_active = false;
	ui->terminal_zoom = 2U;
	ui->disk_active = false;
	ui->disk_sheet = (triage_disk_sheet_t) { 0 };
	for (uint32_t index = 0U; index < (uint32_t)TRIAGE_DISK_BUTTON_COUNT; index++) {
		ui->disk_buttons[index] = (triage_rect_t) { 0, 0, 0U, 0U };
		ui->disk_button_hovered[index] = false;
		ui->disk_button_pressed[index] = false;
	}

	ui->disk_partition_count = 0U;
	ui->disk_space_valid = false;
	ui->terminal_input_length = 0U;
	ui->terminal_input[0] = '\0';
	ui->terminal_line_count = 0U;
	ui->terminal_history_count = 0U;
	ui->terminal_history_index = -1;
	ui->damage = (triage_damage_t) { 0 };
	triage_shell_init(&ui->shell, triage_shell_output, ui);
	triage_layout(ui);
	triage_mark_full(ui);
	return true;
}

void
triage_ui_render(triage_ui_t *ui)
{
	if (ui == 0) {
		return;
	}

	if (ui->full_redraw) {
		startup_options_ui_canvas_fill_vertical_gradient(&ui->canvas, TRIAGE_BG_TOP, TRIAGE_BG_BOTTOM);
		startup_options_ui_canvas_fill_rect(&ui->canvas, 0, 0, ui->canvas.width, TRIAGE_BAR_HEIGHT, TRIAGE_BAR);
		int32_t bar_text_y = triage_text_y(0, TRIAGE_BAR_HEIGHT, 2U);
		startup_options_ui_draw_text(&ui->canvas, 18, bar_text_y, "triageOS", TRIAGE_TEXT, 2U);
		const char *section = ui->terminal_active ? "Terminal" : (ui->disk_active ? "Disk Utility" : "Recovery");
		startup_options_ui_draw_text(&ui->canvas, 124, bar_text_y, section, TRIAGE_MUTED, 2U);
		if (ui->terminal_active) {
			triage_draw_terminal_body(ui);
			triage_draw_terminal_input(ui);
		} else if (ui->disk_active) {
			triage_draw_disk_utility(ui);
		} else {
			triage_draw_static(ui);
			for (uint32_t index = 0U; index < 4U; index++) {
				triage_draw_button(ui, index);
			}

			triage_draw_continue(ui);
			if (ui->modal_action != TRIAGE_ACTION_NONE) {
				triage_draw_modal(ui);
			}
		}

		ui->full_redraw = false;
		ui->dirty_disk = false;
		ui->dirty_disk_buttons = 0U;
		ui->dirty_sheet = false;
		ui->dirty_buttons = 0U;
		ui->dirty_continue = false;
		ui->dirty_modal = false;
		ui->dirty_terminal_body = false;
		ui->dirty_terminal_input = false;
		return;
	}

	if (ui->terminal_active) {
		if (ui->dirty_terminal_body) {
			triage_draw_terminal_body(ui);
			int32_t x; int32_t y; uint32_t width; uint32_t height;
			triage_workspace(ui, &x, &y, &width, &height);
			triage_mark_rect(ui, x, y, width, height);
			ui->dirty_terminal_body = false;
		}

		if (ui->dirty_terminal_input) {
			triage_draw_terminal_input(ui);
			ui->dirty_terminal_input = false;
		}

		return;
	}

	if (ui->disk_active) {
		if (ui->dirty_disk) {
			/* Draws the sheet too, backdrop included, when one is open. */
			triage_draw_disk_utility(ui);
			ui->dirty_disk = false;
			ui->dirty_disk_buttons = 0U;
			ui->dirty_sheet = false;
		}

		for (uint32_t index = 0U; index < (uint32_t)TRIAGE_DISK_BUTTON_COUNT; index++) {
			if ((ui->dirty_disk_buttons & (uint8_t)(1U << index)) != 0U) {
				triage_draw_disk_button(ui, index);
			}
		}

		ui->dirty_disk_buttons = 0U;
		if (ui->disk_sheet.active && ui->dirty_sheet) {
			triage_draw_sheet_body(ui, false);
			ui->dirty_sheet = false;
		}

		return;
	}

	if (ui->modal_action != TRIAGE_ACTION_NONE) {
		if (ui->dirty_modal) {
			triage_draw_modal_button(ui);
			ui->dirty_modal = false;
		}

		return;
	}

	for (uint32_t index = 0U; index < 4U; index++) {
		if ((ui->dirty_buttons & (uint8_t)(1U << index)) != 0U) {
			triage_draw_button(ui, index);
		}
	}

	ui->dirty_buttons = 0U;
	if (ui->dirty_continue) {
		triage_draw_continue(ui);
		ui->dirty_continue = false;
	}
}

void
triage_ui_force_redraw(triage_ui_t *ui)
{
	if (ui != 0) {
		triage_mark_full(ui);
	}
}

bool
triage_ui_pointer_move(triage_ui_t *ui, int32_t x, int32_t y)
{
	if (ui == 0 || ui->terminal_active) {
		return false;
	}

	if (ui->disk_active) {
		if (ui->disk_sheet.active) {
			return triage_sheet_pointer_move(ui, x, y);
		}

		int32_t hovered = triage_disk_button_at(ui, x, y);
		bool changed = false;
		for (uint32_t index = 0U; index < (uint32_t)TRIAGE_DISK_BUTTON_COUNT; index++) {
			bool now = (int32_t)index == hovered;
			if (now == ui->disk_button_hovered[index]) {
				continue;
			}

			ui->disk_button_hovered[index] = now;
			triage_mark_disk_button(ui, index);
			changed = true;
		}

		return changed;
	}

	if (ui->modal_action != TRIAGE_ACTION_NONE) {
		bool hovered = triage_modal_button_hit(ui, x, y);
		if (hovered == ui->modal_button_hovered) {
			return false;
		}

		ui->modal_button_hovered = hovered;
		ui->dirty_modal = true;
		triage_mark_rect(ui, ui->modal_button_x, ui->modal_button_y, ui->modal_button_width, ui->modal_button_height);
		return true;
	}

	int32_t old_hovered = ui->hovered_button;
	bool old_continue = ui->continue_button_hovered;
	ui->hovered_button = triage_hit_button(ui, x, y);
	ui->continue_button_hovered = triage_continue_hit(ui, x, y);
	if (old_hovered == ui->hovered_button && old_continue == ui->continue_button_hovered) {
		return false;
	}

	triage_mark_button(ui, old_hovered);
	triage_mark_button(ui, ui->hovered_button);
	if (old_continue != ui->continue_button_hovered) {
		triage_mark_continue(ui);
	}

	return true;
}

bool
triage_ui_pointer_down(triage_ui_t *ui, int32_t x, int32_t y)
{
	if (ui == 0 || ui->terminal_active) {
		return false;
	}

	if (ui->disk_active) {
		if (ui->disk_sheet.active) {
			return triage_sheet_pointer_down(ui, x, y);
		}

		int32_t pressed = triage_disk_button_at(ui, x, y);
		for (uint32_t index = 0U; index < (uint32_t)TRIAGE_DISK_BUTTON_COUNT; index++) {
			bool now = (int32_t)index == pressed;
			if (now == ui->disk_button_pressed[index]) {
				continue;
			}

			ui->disk_button_pressed[index] = now;
			triage_mark_disk_button(ui, index);
		}

		if (pressed >= 0) {
			return true;
		}

		uint32_t partition_index;
		if (triage_disk_partition_hit(ui, x, y, &partition_index)) {
			ui->disk_selected_partition = partition_index;
			triage_disk_cancel_destructive(ui);
			triage_mark_disk(ui);
			return true;
		}

		return false;
	}

	if (ui->modal_action != TRIAGE_ACTION_NONE) {
		ui->modal_button_pressed = triage_modal_button_hit(ui, x, y);
		if (!ui->modal_button_pressed) {
			return false;
		}

		ui->dirty_modal = true;
		triage_mark_rect(ui, ui->modal_button_x, ui->modal_button_y, ui->modal_button_width, ui->modal_button_height);
		return true;
	}

	ui->continue_button_pressed = triage_continue_hit(ui, x, y);
	if (ui->continue_button_pressed) {
		triage_mark_continue(ui);
		return true;
	}

	int32_t old = ui->selected_button;
	ui->pressed_button = triage_hit_button(ui, x, y);
	if (ui->pressed_button < 0) {
		return false;
	}

	ui->selected_button = ui->pressed_button;
	triage_mark_button(ui, old);
	triage_mark_button(ui, ui->pressed_button);
	return true;
}

bool
triage_ui_pointer_up(triage_ui_t *ui, int32_t x, int32_t y)
{
	if (ui == 0 || ui->terminal_active) {
		return false;
	}

	if (ui->disk_active) {
		if (ui->disk_sheet.active) {
			return triage_sheet_pointer_up(ui, x, y);
		}

		int32_t activated = -1;
		bool changed = false;
		for (uint32_t index = 0U; index < (uint32_t)TRIAGE_DISK_BUTTON_COUNT; index++) {
			if (!ui->disk_button_pressed[index]) {
				continue;
			}

			ui->disk_button_pressed[index] = false;
			triage_mark_disk_button(ui, index);
			changed = true;

			const triage_rect_t *rect = &ui->disk_buttons[index];
			if (startup_options_ui_point_in_squircle(x, y, rect->x, rect->y, rect->width, rect->height, TRIAGE_DISK_BUTTON_RADIUS)) {
				activated = (int32_t)index;
			}
		}

		switch (activated) {
		case TRIAGE_DISK_BUTTON_PRIMARY:
			triage_disk_toggle_mount(ui);
			break;
		case TRIAGE_DISK_BUTTON_REFRESH:
			triage_disk_cancel_destructive(ui);
			triage_disk_refresh(ui);
			triage_disk_set_status(ui, "Disk information refreshed.");
			triage_mark_disk(ui);
			break;
		case TRIAGE_DISK_BUTTON_VERIFY:
			triage_disk_verify(ui);
			break;
		case TRIAGE_DISK_BUTTON_ADD:
			triage_disk_add(ui);
			break;
		case TRIAGE_DISK_BUTTON_DELETE:
			triage_disk_delete(ui);
			break;
		default:
			break;
		}

		return changed;
	}

	if (ui->modal_action != TRIAGE_ACTION_NONE) {
		bool activate = ui->modal_button_pressed && triage_modal_button_hit(ui, x, y);
		bool changed = ui->modal_button_pressed;
		ui->modal_button_pressed = false;
		if (activate) {
			ui->modal_action = TRIAGE_ACTION_NONE;
			ui->modal_custom = false;
			triage_mark_full(ui);
		} else if (changed) {
			ui->dirty_modal = true;
			triage_mark_rect(ui, ui->modal_button_x, ui->modal_button_y, ui->modal_button_width, ui->modal_button_height);
		}

		return changed || activate;
	}

	if (ui->continue_button_pressed) {
		bool activate = triage_continue_hit(ui, x, y);
		ui->continue_button_pressed = false;
		triage_mark_continue(ui);
		if (activate) {
			(void)triage_activate_selected(ui);
		}

		return true;
	}

	int32_t pressed = ui->pressed_button;
	ui->pressed_button = -1;
	if (pressed >= 0) {
		triage_mark_button(ui, pressed);
	}

	return pressed >= 0;
}

bool
triage_ui_key(triage_ui_t *ui, uint32_t code, int32_t value, uint32_t modifiers)
{
	if (ui == 0 || value == 0) {
		return false;
	}

	if (ui->terminal_active) {
		if ((modifiers & STARTUP_OPTIONS_UI_MOD_CTRL) != 0U && code == TRIAGE_KEY_MINUS) {
			if (ui->terminal_zoom > 1U) {
				ui->terminal_zoom--;
				triage_mark_full(ui);
				return true;
			}

			return false;
		}

		if ((modifiers & STARTUP_OPTIONS_UI_MOD_CTRL) != 0U && code == TRIAGE_KEY_EQUAL) {
			if (ui->terminal_zoom < 3U) {
				ui->terminal_zoom++;
				triage_mark_full(ui);
				return true;
			}

			return false;
		}

		if ((modifiers & STARTUP_OPTIONS_UI_MOD_CTRL) != 0U && code == TRIAGE_KEY_ZERO) {
			if (ui->terminal_zoom != 2U) {
				ui->terminal_zoom = 2U;
				triage_mark_full(ui);
				return true;
			}

			return false;
		}

		if ((modifiers & STARTUP_OPTIONS_UI_MOD_CTRL) != 0U && code == 38U) {
			ui->terminal_line_count = 0U;
			ui->dirty_terminal_body = true;
			return true;
		}

		if ((modifiers & STARTUP_OPTIONS_UI_MOD_CTRL) != 0U && code == 46U) {
			triage_terminal_push(ui, "^C");
			ui->terminal_input_length = 0U;
			ui->terminal_input[0] = '\0';
			ui->dirty_terminal_input = true;
			return true;
		}

		if (code == TRIAGE_KEY_ESCAPE) {
			ui->terminal_active = false;
			triage_mark_full(ui);
			return true;
		}

		if (code == TRIAGE_KEY_ENTER) {
			triage_terminal_execute(ui);
			return true;
		}

		if (code == TRIAGE_KEY_UP) {
			triage_terminal_history_move(ui, -1);
			return true;
		}

		if (code == TRIAGE_KEY_DOWN) {
			triage_terminal_history_move(ui, 1);
			return true;
		}

		if (code == TRIAGE_KEY_BACKSPACE) {
			if (ui->terminal_input_length == 0U) {
				return false;
			}

			ui->terminal_input[--ui->terminal_input_length] = '\0';
			ui->dirty_terminal_input = true;
			return true;
		}

		char character = triage_key_character(code, modifiers);
		if (character == '\0' || ui->terminal_input_length + 1U >= TRIAGE_TERMINAL_INPUT_MAX) {
			return false;
		}

		ui->terminal_input[ui->terminal_input_length++] = character;
		ui->terminal_input[ui->terminal_input_length] = '\0';
		ui->dirty_terminal_input = true;
		return true;
	}

	if (ui->disk_active) {
		if (ui->disk_sheet.active) {
			return triage_sheet_key(ui, code, modifiers);
		}

		if (code == TRIAGE_KEY_ESCAPE) {
			if (ui->disk_initialize_armed || ui->disk_delete_armed) {
				triage_disk_cancel_destructive(ui);
				triage_disk_set_status(ui, "Destructive action cancelled.");
				triage_mark_disk(ui);
				return true;
			}

			/* Leaving retitles the bar and brings the menu back. */
			ui->disk_active = false;
			triage_mark_full(ui);
			return true;
		}

		if (code == TRIAGE_KEY_R) {
			triage_disk_cancel_destructive(ui);
			triage_disk_refresh(ui);
			triage_disk_set_status(ui, "Disk information refreshed.");
			triage_mark_disk(ui);
			return true;
		}

		if (code == TRIAGE_KEY_UP && ui->disk_partition_count != 0U) {
			triage_disk_cancel_destructive(ui);
			ui->disk_selected_partition = ui->disk_selected_partition == 0U
				? ui->disk_partition_count - 1U
				: ui->disk_selected_partition - 1U;
			triage_mark_disk(ui);
			return true;
		}

		if (code == TRIAGE_KEY_DOWN && ui->disk_partition_count != 0U) {
			triage_disk_cancel_destructive(ui);
			ui->disk_selected_partition = (ui->disk_selected_partition + 1U) % ui->disk_partition_count;
			triage_mark_disk(ui);
			return true;
		}

		return false;
	}

	if (ui->modal_action != TRIAGE_ACTION_NONE) {
		if (code != TRIAGE_KEY_ESCAPE && code != TRIAGE_KEY_ENTER) {
			return false;
		}

		ui->modal_action = TRIAGE_ACTION_NONE;
		ui->modal_custom = false;
		ui->modal_button_pressed = false;
		triage_mark_full(ui);
		return true;
	}

	if (code == TRIAGE_KEY_UP) {
		int32_t old = ui->selected_button;
		ui->selected_button = ui->selected_button <= 0 ? 3 : ui->selected_button - 1;
		triage_mark_button(ui, old);
		triage_mark_button(ui, ui->selected_button);
		return true;
	}

	if (code == TRIAGE_KEY_DOWN || code == TRIAGE_KEY_TAB) {
		int32_t old = ui->selected_button;
		ui->selected_button = (ui->selected_button + 1) % 4;
		triage_mark_button(ui, old);
		triage_mark_button(ui, ui->selected_button);
		return true;
	}

	if (code == TRIAGE_KEY_ENTER) {
		return triage_activate_selected(ui);
	}

	return false;
}

bool
triage_ui_take_damage(triage_ui_t *ui, triage_damage_t *damage)
{
	if (ui == 0 || damage == 0 || !ui->damage.valid) {
		return false;
	}

	*damage = ui->damage;
	ui->damage = (triage_damage_t) { 0 };
	return true;
}

triage_action_t
triage_ui_take_action(triage_ui_t *ui)
{
	if (ui == 0) {
		return TRIAGE_ACTION_NONE;
	}

	triage_action_t action = ui->pending_action;
	ui->pending_action = TRIAGE_ACTION_NONE;
	return action;
}

void
triage_ui_show_modal(triage_ui_t *ui, triage_action_t action)
{
	if (ui == 0 || action == TRIAGE_ACTION_NONE) {
		return;
	}

	ui->terminal_active = false;
	ui->disk_active = false;
	ui->disk_sheet.active = false;
	ui->modal_custom = false;
	ui->modal_action = action;
	ui->modal_button_hovered = false;
	ui->modal_button_pressed = false;
	triage_mark_full(ui);
}

void
triage_ui_show_message(triage_ui_t *ui, const char *title, const char *message)
{
	if (ui == 0 || title == 0 || message == 0) {
		return;
	}

	ui->terminal_active = false;
	ui->disk_active = false;
	ui->disk_sheet.active = false;
	ui->modal_custom = true;
	ui->modal_action = TRIAGE_ACTION_REINSTALL;
	triage_copy(ui->modal_title, sizeof(ui->modal_title), title);
	triage_copy(ui->modal_message, sizeof(ui->modal_message), message);
	ui->modal_button_hovered = false;
	ui->modal_button_pressed = false;
	triage_mark_full(ui);
}

void
triage_ui_terminal_write(triage_ui_t *ui, const char *text)
{
	if (ui != 0 && ui->terminal_active) {
		triage_terminal_push(ui, text);
	}
}
