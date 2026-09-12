#include <kern/boot/splash.h>

#include <arch/arm64/timer.h>
#include <kern/boot/splash_asset.h>
#include <kern/console/console.h>
#include <kern/console/font8x16.h>

#include <stdbool.h>
#include <stdint.h>

#define BOOT_SPLASH_BACKGROUND 0x00000000U
#define BOOT_SPLASH_MAX_SCALE 1U
#define BOOT_SPLASH_BAR_WIDTH 260U
#define BOOT_SPLASH_BAR_HEIGHT 6U
#define BOOT_SPLASH_BAR_GAP 44U
#define BOOT_SPLASH_FRAME_HZ 30U
#define BOOT_SPLASH_BAR_TRACK 0x00303034U
#define BOOT_SPLASH_BAR_FILL 0x00FFFFFFU
#define BOOT_SPLASH_PROGRESS_MAX 1000U
#define BOOT_SPLASH_FINISH_MIN_STEP 24U
#define BOOT_SPLASH_STATUS_GAP 18U
#define BOOT_SPLASH_STATUS_HEIGHT 18U
#define BOOT_SPLASH_STATUS_SCALE 2U
#define BOOT_SPLASH_STATUS_COLOR 0x00D8D8DEU
#define BOOT_SPLASH_STATUS_MAX_LENGTH 63U

/*
 * Verbose serial log rendered directly onto the splash scanout, underneath the
 * logo/bar/status layer -- the same "SRD" look as an Apple internal boot: every
 * line that would normally only reach UART scrolls across the framebuffer while
 * the boot logo stays fixed on top of it.
 */
#define BOOT_SPLASH_TEXT_CELL_WIDTH FONT8X16_WIDTH
#define BOOT_SPLASH_TEXT_CELL_HEIGHT FONT8X16_HEIGHT
#define BOOT_SPLASH_TEXT_COLOR 0x00565660U

static display_device_t *g_display;
static uint64_t g_started_at;
static uint32_t g_scale;
static uint32_t g_top;
static uint32_t g_progress;
static bool g_visible;

static char g_status_text[BOOT_SPLASH_STATUS_MAX_LENGTH + 1U];
static bool g_status_active;

static uint32_t g_text_columns;
static uint32_t g_text_rows;
static uint32_t g_text_cursor_x;
static uint32_t g_text_cursor_y;
static bool g_text_active;
static bool g_text_dirty;
static bool g_text_replaying;

static const uint8_t g_status_glyph_space[7] = { 0U, 0U, 0U, 0U, 0U, 0U, 0U };
static const uint8_t g_status_glyph_dot[7] = { 0U, 0U, 0U, 0U, 0U, 0x0CU, 0x0CU };
static const uint8_t g_status_glyph_l[7] = { 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x1FU };
static const uint8_t g_status_glyph_a[7] = { 0U, 0x0EU, 0x01U, 0x0FU, 0x11U, 0x13U, 0x0DU };
static const uint8_t g_status_glyph_d[7] = { 0x01U, 0x01U, 0x0DU, 0x13U, 0x11U, 0x13U, 0x0DU };
static const uint8_t g_status_glyph_g[7] = { 0U, 0x0EU, 0x11U, 0x11U, 0x0FU, 0x01U, 0x0EU };
static const uint8_t g_status_glyph_i[7] = { 0x04U, 0U, 0x0CU, 0x04U, 0x04U, 0x04U, 0x0EU };
static const uint8_t g_status_glyph_n[7] = { 0U, 0U, 0x1EU, 0x11U, 0x11U, 0x11U, 0x11U };
static const uint8_t g_status_glyph_o[7] = { 0U, 0U, 0x0EU, 0x11U, 0x11U, 0x11U, 0x0EU };
static const uint8_t g_status_glyph_p[7] = { 0U, 0x1EU, 0x11U, 0x11U, 0x1EU, 0x10U, 0x10U };
static const uint8_t g_status_glyph_r[7] = { 0U, 0U, 0x16U, 0x19U, 0x10U, 0x10U, 0x10U };
static const uint8_t g_status_glyph_s[7] = { 0U, 0x0FU, 0x10U, 0x0EU, 0x01U, 0x01U, 0x1EU };
static const uint8_t g_status_glyph_t[7] = { 0x04U, 0x04U, 0x0EU, 0x04U, 0x04U, 0x05U, 0x02U };
static const uint8_t g_status_glyph_u[7] = { 0U, 0U, 0x11U, 0x11U, 0x11U, 0x13U, 0x0DU };

static const uint8_t *boot_splash_status_glyph(char character)
{
	switch (character) {
	case 'L': case 'l': return g_status_glyph_l;
	case 'a': return g_status_glyph_a;
	case 'd': return g_status_glyph_d;
	case 'g': return g_status_glyph_g;
	case 'i': return g_status_glyph_i;
	case 'n': return g_status_glyph_n;
	case 'o': return g_status_glyph_o;
	case 'p': return g_status_glyph_p;
	case 'r': return g_status_glyph_r;
	case 's': return g_status_glyph_s;
	case 't': return g_status_glyph_t;
	case 'u': return g_status_glyph_u;
	case '.': return g_status_glyph_dot;
	case ' ': return g_status_glyph_space;
	default: return g_status_glyph_space;
	}
}

static uint32_t boot_splash_blend(uint32_t destination, uint32_t source)
{
	uint32_t alpha = source >> 24U;
	if (alpha == 0U) return destination;
	if (alpha == 255U) return source & 0x00FFFFFFU;

	uint32_t inverse = 255U - alpha;
	uint32_t source_red = (source >> 16U) & 0xFFU;
	uint32_t source_green = (source >> 8U) & 0xFFU;
	uint32_t source_blue = source & 0xFFU;
	uint32_t destination_red = (destination >> 16U) & 0xFFU;
	uint32_t destination_green = (destination >> 8U) & 0xFFU;
	uint32_t destination_blue = destination & 0xFFU;
	uint32_t red = (source_red * alpha + destination_red * inverse + 127U) / 255U;
	uint32_t green = (source_green * alpha + destination_green * inverse + 127U) / 255U;
	uint32_t blue = (source_blue * alpha + destination_blue * inverse + 127U) / 255U;
	return (red << 16U) | (green << 8U) | blue;
}

static uint64_t boot_splash_ticks_for_ms(uint64_t milliseconds)
{
	uint64_t frequency = timer_get_frequency();
	return (milliseconds / 1000ULL) * frequency + (milliseconds % 1000ULL) * frequency / 1000ULL;
}

static bool boot_splash_bar_rect(uint32_t *left, uint32_t *top)
{
	if (g_display == 0 || left == 0 || top == 0) return false;
	uint32_t scaled_height = g_boot_splash_height * g_scale;
	uint32_t center_x = g_display->width / 2U;
	uint32_t bar_top = g_top + scaled_height + BOOT_SPLASH_BAR_GAP;
	uint32_t half = BOOT_SPLASH_BAR_WIDTH / 2U;
	if (center_x < half || center_x + half > g_display->width || bar_top + BOOT_SPLASH_BAR_HEIGHT > g_display->height) return false;
	*left = center_x - half;
	*top = bar_top;
	return true;
}

static bool boot_splash_status_rect(uint32_t *top)
{
	uint32_t left;
	uint32_t bar_top;
	if (top == 0 || !boot_splash_bar_rect(&left, &bar_top)) return false;
	(void)left;
	uint32_t status_top = bar_top + BOOT_SPLASH_BAR_HEIGHT + BOOT_SPLASH_STATUS_GAP;
	if (g_display == 0 || status_top + BOOT_SPLASH_STATUS_HEIGHT > g_display->height) return false;
	*top = status_top;
	return true;
}

static uint32_t boot_splash_status_width(const char *status)
{
	if (status == 0) return 0U;
	uint32_t count = 0U;
	while (status[count] != '\0') count++;
	if (count == 0U) return 0U;
	uint32_t glyph_width = 5U * BOOT_SPLASH_STATUS_SCALE;
	uint32_t spacing = BOOT_SPLASH_STATUS_SCALE;
	return count * glyph_width + (count - 1U) * spacing;
}

/*
 * Paint the cached status caption from g_status_text without presenting.
 * Reused both by boot_splash_set_status() and by the text-overlay scroll
 * handler, which must restore the caption after a full-screen scroll wipes it.
 */
static bool boot_splash_paint_status(void)
{
	if (!g_status_active || g_display == 0) return false;
	uint32_t top;
	if (!boot_splash_status_rect(&top)) return false;

	for (uint32_t y = 0U; y < BOOT_SPLASH_STATUS_HEIGHT; y++) {
		uint32_t *row = g_display->framebuffer + (uint64_t)(top + y) * g_display->stride;
		for (uint32_t x = 0U; x < g_display->width; x++) row[x] = BOOT_SPLASH_BACKGROUND;
	}

	const char *status = g_status_text;
	uint32_t width = boot_splash_status_width(status);
	uint32_t cursor = width < g_display->width ? (g_display->width - width) / 2U : 0U;
	uint32_t glyph_top = top + (BOOT_SPLASH_STATUS_HEIGHT - 7U * BOOT_SPLASH_STATUS_SCALE) / 2U;

	for (uint32_t index = 0U; status[index] != '\0'; index++) {
		const uint8_t *glyph = boot_splash_status_glyph(status[index]);
		for (uint32_t row = 0U; row < 7U; row++) {
			for (uint32_t column = 0U; column < 5U; column++) {
				if ((glyph[row] & (1U << (4U - column))) == 0U) continue;
				for (uint32_t dy = 0U; dy < BOOT_SPLASH_STATUS_SCALE; dy++) {
					uint32_t *pixels = g_display->framebuffer + (uint64_t)(glyph_top + row * BOOT_SPLASH_STATUS_SCALE + dy) * g_display->stride;
					for (uint32_t dx = 0U; dx < BOOT_SPLASH_STATUS_SCALE; dx++) {
						uint32_t x = cursor + column * BOOT_SPLASH_STATUS_SCALE + dx;
						if (x < g_display->width) pixels[x] = BOOT_SPLASH_STATUS_COLOR;
					}
				}
			}
		}

		cursor += 6U * BOOT_SPLASH_STATUS_SCALE;
	}

	return true;
}

bool boot_splash_set_status(const char *status)
{
	if (!g_visible || g_display == 0 || status == 0) return false;
	uint32_t top;
	if (!boot_splash_status_rect(&top)) return false;

	uint32_t length = 0U;
	while (status[length] != '\0' && length < BOOT_SPLASH_STATUS_MAX_LENGTH) length++;
	for (uint32_t index = 0U; index < length; index++) g_status_text[index] = status[index];
	g_status_text[length] = '\0';
	g_status_active = true;

	if (!boot_splash_paint_status()) return false;
	return display_present(g_display, 0U, top, g_display->width, BOOT_SPLASH_STATUS_HEIGHT);
}

/* Paint the progress bar at its current fill level without presenting. */
static bool boot_splash_paint_bar(void)
{
	if (g_display == 0) return false;

	uint32_t left;
	uint32_t top;
	if (!boot_splash_bar_rect(&left, &top)) return false;
	uint32_t filled = (BOOT_SPLASH_BAR_WIDTH * g_progress) / BOOT_SPLASH_PROGRESS_MAX;
	if (filled > BOOT_SPLASH_BAR_WIDTH) filled = BOOT_SPLASH_BAR_WIDTH;

	for (uint32_t y = 0U; y < BOOT_SPLASH_BAR_HEIGHT; y++) {
		uint32_t *row = g_display->framebuffer + (uint64_t)(top + y) * g_display->stride + left;
		bool edge = y == 0U || y == BOOT_SPLASH_BAR_HEIGHT - 1U;
		uint32_t inset = edge ? 1U : 0U;

		for (uint32_t x = 0U; x < BOOT_SPLASH_BAR_WIDTH; x++) {
			if (x < inset || x + inset >= BOOT_SPLASH_BAR_WIDTH) {
				row[x] = BOOT_SPLASH_BACKGROUND;
				continue;
			}

			row[x] = x < filled ? BOOT_SPLASH_BAR_FILL : BOOT_SPLASH_BAR_TRACK;
		}
	}

	return true;
}

static bool boot_splash_draw_bar(void)
{
	if (!g_visible || g_display == 0) return false;

	uint32_t left;
	uint32_t top;
	if (!boot_splash_bar_rect(&left, &top)) return false;
	if (!boot_splash_paint_bar()) return false;

	return display_present(g_display, left, top, BOOT_SPLASH_BAR_WIDTH, BOOT_SPLASH_BAR_HEIGHT);
}

static uint32_t g_left;

/* Paint the boot logo at its fixed position without presenting. */
static void boot_splash_paint_logo(void)
{
	if (g_display == 0) return;

	for (uint32_t source_y = 0U; source_y < g_boot_splash_height; source_y++) {
		for (uint32_t source_x = 0U; source_x < g_boot_splash_width; source_x++) {
			uint32_t source = g_boot_splash_pixels[(uint64_t)source_y * g_boot_splash_width + source_x];
			if ((source >> 24U) == 0U) continue;
			uint32_t destination_x = g_left + source_x * g_scale;
			uint32_t destination_y = g_top + source_y * g_scale;

			for (uint32_t dy = 0U; dy < g_scale; dy++) {
				uint32_t *row = g_display->framebuffer + (uint64_t)(destination_y + dy) * g_display->stride + destination_x;
				for (uint32_t dx = 0U; dx < g_scale; dx++) row[dx] = boot_splash_blend(row[dx], source);
			}
		}
	}
}

/*
 * Recomposite the logo, progress bar and status caption on top of the
 * scrolling text console. Called after every full-screen text scroll, since
 * the scroll shifts every pixel row -- including the fixed overlay -- up by
 * one text line.
 */
static void boot_splash_composite_overlay(void)
{
	if (g_display == 0) return;
	boot_splash_paint_logo();
	(void)boot_splash_paint_bar();
	(void)boot_splash_paint_status();
}

static void boot_splash_text_clear_cell(uint32_t column, uint32_t row)
{
	uint32_t start_x = column * BOOT_SPLASH_TEXT_CELL_WIDTH;
	uint32_t start_y = row * BOOT_SPLASH_TEXT_CELL_HEIGHT;

	for (uint32_t y = 0U; y < BOOT_SPLASH_TEXT_CELL_HEIGHT; y++) {
		uint32_t *pixels = g_display->framebuffer + (uint64_t)(start_y + y) * g_display->stride + start_x;
		for (uint32_t x = 0U; x < BOOT_SPLASH_TEXT_CELL_WIDTH; x++) pixels[x] = BOOT_SPLASH_BACKGROUND;
	}
}

static void boot_splash_text_draw_char(uint32_t column, uint32_t row, char character)
{
	if (character < 32 || character > 126) character = '?';

	uint32_t start_x = column * BOOT_SPLASH_TEXT_CELL_WIDTH;
	uint32_t start_y = row * BOOT_SPLASH_TEXT_CELL_HEIGHT;
	const uint8_t *glyph = &g_font8x16[(uint32_t)(uint8_t)character * FONT8X16_HEIGHT];

	boot_splash_text_clear_cell(column, row);

	for (uint32_t glyph_row = 0U; glyph_row < FONT8X16_HEIGHT; glyph_row++) {
		uint8_t bits = glyph[glyph_row];
		uint32_t *pixels = g_display->framebuffer + (uint64_t)(start_y + glyph_row) * g_display->stride + start_x;

		for (uint32_t column_bit = 0U; column_bit < FONT8X16_WIDTH; column_bit++) {
			if ((bits & (1U << column_bit)) == 0U) continue;
			pixels[column_bit] = BOOT_SPLASH_TEXT_COLOR;
		}
	}
}

/*
 * Shift the active text rows up by one cell height and clear the vacated
 * line, exactly like a terminal scroll -- except this operates directly on
 * the splash's live scanout, so the logo/bar/status must be repainted on top
 * once the shift is done.
 */
static void boot_splash_text_scroll(void)
{
	uint32_t active_height = g_text_rows * BOOT_SPLASH_TEXT_CELL_HEIGHT;
	uint32_t pixel_rows = active_height - BOOT_SPLASH_TEXT_CELL_HEIGHT;

	for (uint32_t y = 0U; y < pixel_rows; y++) {
		uint32_t *destination = g_display->framebuffer + (uint64_t)y * g_display->stride;
		uint32_t *source = g_display->framebuffer + (uint64_t)(y + BOOT_SPLASH_TEXT_CELL_HEIGHT) * g_display->stride;
		for (uint32_t x = 0U; x < g_display->width; x++) destination[x] = source[x];
	}

	for (uint32_t y = pixel_rows; y < active_height; y++) {
		uint32_t *row = g_display->framebuffer + (uint64_t)y * g_display->stride;
		for (uint32_t x = 0U; x < g_display->width; x++) row[x] = BOOT_SPLASH_BACKGROUND;
	}

	boot_splash_composite_overlay();
}

static void boot_splash_text_advance_line(void)
{
	g_text_cursor_x = 0U;
	g_text_cursor_y++;

	if (g_text_cursor_y >= g_text_rows) {
		boot_splash_text_scroll();
		g_text_cursor_y = g_text_rows - 1U;
	}
}

/* Present the full screen once if the text console has drawn since the last flush. */
static bool boot_splash_text_flush(void)
{
	if (!g_text_dirty || g_display == 0) return true;
	g_text_dirty = false;
	return display_present_full(g_display);
}

/*
 * kconsole sink that mirrors every character written to kputc/kprintf (i.e.
 * everything the serial/UART sink also receives) onto the splash scanout as
 * scrolling monospace text, underneath the logo/bar/status overlay.
 */
static void boot_splash_text_putc(char character, void *context)
{
	(void)context;
	if (!g_text_active) return;

	if (character == '\r') {
		g_text_cursor_x = 0U;
		return;
	}

	if (character == '\n') {
		boot_splash_text_advance_line();
		g_text_dirty = true;
		/*
		 * Flush immediately on every completed line. Most of the kernel's
		 * boot log happens between boot_splash_wait() returning and
		 * boot_splash_finish() running, a long synchronous stretch with no
		 * timer-driven present of its own -- without this the scrolling log
		 * would never actually reach the scanout during that window. Skipped
		 * during the one-time history replay in boot_splash_show(), which
		 * would otherwise fire one full-screen present per retained line.
		 */
		if (!g_text_replaying) (void)boot_splash_text_flush();
		return;
	}

	if (character == '\t') {
		uint32_t spaces = 4U - (g_text_cursor_x % 4U);
		for (uint32_t index = 0U; index < spaces; index++) boot_splash_text_putc(' ', context);
		return;
	}

	if (g_text_cursor_x >= g_text_columns) boot_splash_text_advance_line();

	boot_splash_text_draw_char(g_text_cursor_x, g_text_cursor_y, character);
	g_text_cursor_x++;
	g_text_dirty = true;
}

static void boot_splash_text_init(void)
{
	if (g_display == 0) {
		g_text_active = false;
		return;
	}

	g_text_columns = g_display->width / BOOT_SPLASH_TEXT_CELL_WIDTH;
	g_text_rows = g_display->height / BOOT_SPLASH_TEXT_CELL_HEIGHT;
	g_text_cursor_x = 0U;
	g_text_cursor_y = 0U;
	g_text_dirty = false;
	g_text_active = g_text_columns != 0U && g_text_rows != 0U;
}

bool boot_splash_show(display_device_t *display)
{
	if (display == 0 || !display->registered || display->framebuffer == 0 || display->bytes_per_pixel != sizeof(uint32_t)) return false;
	if (g_boot_splash_width == 0U || g_boot_splash_height == 0U || timer_get_frequency() == 0ULL) return false;

	for (uint32_t y = 0U; y < display->height; y++) {
		uint32_t *row = display->framebuffer + (uint64_t)y * display->stride;
		for (uint32_t x = 0U; x < display->width; x++) row[x] = BOOT_SPLASH_BACKGROUND;
	}

	uint32_t scale_x = display->width / g_boot_splash_width;
	uint32_t scale_y = display->height / g_boot_splash_height;
	g_scale = scale_x < scale_y ? scale_x : scale_y;
	if (g_scale == 0U) g_scale = 1U;
	if (g_scale > BOOT_SPLASH_MAX_SCALE) g_scale = BOOT_SPLASH_MAX_SCALE;

	uint32_t scaled_width = g_boot_splash_width * g_scale;
	uint32_t scaled_height = g_boot_splash_height * g_scale;
	g_left = (display->width - scaled_width) / 2U;
	g_top = (display->height - scaled_height) / 2U;

	g_display = display;
	g_status_active = false;

	boot_splash_text_init();
	if (g_text_active) {
		/*
		 * Replay the retained console history so the splash appears with the
		 * boot log already dense and scrolled behind the logo -- the "SRD"
		 * look -- rather than starting blank and only filling in from here.
		 * g_text_replaying suppresses the per-line present during replay
		 * (see boot_splash_text_putc) so this doesn't fire one full-screen
		 * present per retained line; the present below shows the end result.
		 */
		g_text_replaying = true;
		bool registered = kconsole_register_sink(boot_splash_text_putc, 0, true);
		g_text_replaying = false;
		if (!registered) g_text_active = false;
	}

	boot_splash_paint_logo();

	g_started_at = timer_get_ticks();
	g_progress = 80U;
	g_visible = true;
	if (!boot_splash_draw_bar()) return false;
	if (!display_present_full(display)) return false;
	kprintf("boot_splash: shown %ux%u at %ux scale\n", g_boot_splash_width, g_boot_splash_height, g_scale);
	return true;
}

bool boot_splash_wait(uint64_t milliseconds, boot_splash_service_t service)
{
	if (!g_visible || g_display == 0) return false;
	uint64_t frequency = timer_get_frequency();
	if (frequency == 0ULL) return false;

	uint64_t deadline = g_started_at + boot_splash_ticks_for_ms(milliseconds);
	uint64_t frame_interval = frequency / BOOT_SPLASH_FRAME_HZ;
	if (frame_interval == 0ULL) frame_interval = 1ULL;
	uint64_t next_frame = timer_get_ticks();

	while (timer_get_ticks() < deadline) {
		uint64_t now = timer_get_ticks();
		if (now >= next_frame) {
			if (service != 0) service();
			uint32_t remaining = 820U > g_progress ? 820U - g_progress : 0U;
			uint32_t step = remaining / 24U;
			if (remaining != 0U) g_progress += step != 0U ? step : 1U;
			if (!boot_splash_draw_bar()) return false;
			if (!boot_splash_text_flush()) return false;
			next_frame = now + frame_interval;
		}
		__asm__ volatile("yield");
	}

	return true;
}

bool boot_splash_finish(void)
{
	if (!g_visible || g_display == 0) return false;
	uint64_t frequency = timer_get_frequency();
	if (frequency == 0ULL) return false;
	uint64_t frame_interval = frequency / BOOT_SPLASH_FRAME_HZ;
	if (frame_interval == 0ULL) frame_interval = 1ULL;

	/*
	 * Stop mirroring the serial log onto the scanout before UI takes it over;
	 * the completion animation below finishes without further text scrolling.
	 */
	if (g_text_active) {
		kconsole_unregister_sink(boot_splash_text_putc, 0);
		g_text_active = false;
	}

	while (g_progress < BOOT_SPLASH_PROGRESS_MAX) {
		uint32_t remaining = BOOT_SPLASH_PROGRESS_MAX - g_progress;
		uint32_t step = remaining / 4U;
		if (step < BOOT_SPLASH_FINISH_MIN_STEP) step = BOOT_SPLASH_FINISH_MIN_STEP;
		g_progress += step < remaining ? step : remaining;
		if (!boot_splash_draw_bar()) return false;
		uint64_t deadline = timer_get_ticks() + frame_interval;
		while (timer_get_ticks() < deadline) __asm__ volatile("yield");
	}

	kputln("boot_splash: progress complete");
	return true;
}
