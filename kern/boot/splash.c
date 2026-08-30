#include <kern/boot/splash.h>

#include <arch/arm64/timer.h>
#include <kern/boot/splash_asset.h>
#include <kern/console/console.h>

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

static display_device_t *g_display;
static uint64_t g_started_at;
static uint32_t g_scale;
static uint32_t g_top;
static uint32_t g_progress;
static bool g_visible;

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

bool boot_splash_set_status(const char *status)
{
	if (!g_visible || g_display == 0 || status == 0) return false;
	uint32_t top;
	if (!boot_splash_status_rect(&top)) return false;

	for (uint32_t y = 0U; y < BOOT_SPLASH_STATUS_HEIGHT; y++) {
		uint32_t *row = g_display->framebuffer + (uint64_t)(top + y) * g_display->stride;
		for (uint32_t x = 0U; x < g_display->width; x++) row[x] = BOOT_SPLASH_BACKGROUND;
	}

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

	return display_present(g_display, 0U, top, g_display->width, BOOT_SPLASH_STATUS_HEIGHT);
}

static bool boot_splash_draw_bar(void)
{
	if (!g_visible || g_display == 0) return false;

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

	return display_present(g_display, left, top, BOOT_SPLASH_BAR_WIDTH, BOOT_SPLASH_BAR_HEIGHT);
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
	uint32_t left = (display->width - scaled_width) / 2U;
	g_top = (display->height - scaled_height) / 2U;

	for (uint32_t source_y = 0U; source_y < g_boot_splash_height; source_y++) {
		for (uint32_t source_x = 0U; source_x < g_boot_splash_width; source_x++) {
			uint32_t source = g_boot_splash_pixels[(uint64_t)source_y * g_boot_splash_width + source_x];
			if ((source >> 24U) == 0U) continue;
			uint32_t destination_x = left + source_x * g_scale;
			uint32_t destination_y = g_top + source_y * g_scale;

			for (uint32_t dy = 0U; dy < g_scale; dy++) {
				uint32_t *row = display->framebuffer + (uint64_t)(destination_y + dy) * display->stride + destination_x;
				for (uint32_t dx = 0U; dx < g_scale; dx++) row[dx] = boot_splash_blend(row[dx], source);
			}
		}
	}

	g_display = display;
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
