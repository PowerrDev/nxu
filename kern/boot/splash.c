#include <kern/boot/splash.h>

#include <kern/arm64/timer.h>
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
/*
 * Deliberately much lower than BOOT_SPLASH_FRAME_HZ: every text-overlay
 * present transfers the whole framebuffer (~3.7MB at 1366x690 XRGB8888), and
 * a burst of log lines a few milliseconds apart can queue full-frame
 * presents faster than an emulated virtio-gpu queue drains them, leaving the
 * screen showing a stale/in-flight frame until the backlog clears.
 */
#define BOOT_SPLASH_TEXT_PRESENT_HZ 8U
#define BOOT_SPLASH_BAR_TRACK 0x00303034U
#define BOOT_SPLASH_BAR_FILL 0x00FFFFFFU
#define BOOT_SPLASH_PROGRESS_MAX 1000U
/*
 * Ceiling boot_splash_text_flush() nudges the bar toward while the log is
 * still streaming in, leaving a visible amount of bar for
 * boot_splash_finish() to close out once loading actually completes -- see
 * both for the full picture.
 */
#define BOOT_SPLASH_LOG_PROGRESS_MAX 950U
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

/*
 * Pixel rectangle reserved for the fixed logo/bar header, computed once in
 * boot_splash_text_init(). Text draws, clears and scroll copies all check
 * this rectangle and skip only the cells/columns that actually overlap it,
 * so the console can use the full screen (both the rows above and below the
 * header, and the columns to its left and right within those same rows)
 * instead of losing whole rows across the entire screen width.
 */
static uint32_t g_header_top;
static uint32_t g_header_bottom;
static uint32_t g_header_left;
static uint32_t g_header_right;
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

/*
 * Paint the boot logo at its fixed position without presenting. Called once,
 * from boot_splash_show(): the scrolling text region lives entirely below
 * the logo/bar (see boot_splash_text_init()), so nothing ever scrolls
 * through or overwrites this rect again after the initial paint.
 */
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
 * Pixel Y of the header's bottom edge -- right below the logo, or below the
 * bar too when it fits -- used by boot_splash_text_init() as the point the
 * scrolling console resumes below the header. The status caption is
 * deliberately not reserved for -- it is rare (only shown while a recovery
 * key is held during the splash hold) and reserving space for it
 * unconditionally would leave a permanent dead gap in the far more common
 * case where it never appears.
 */
static uint32_t boot_splash_text_region_top(void)
{
	uint32_t top = g_top + g_boot_splash_height * g_scale;

	uint32_t bar_left;
	uint32_t bar_top;
	if (boot_splash_bar_rect(&bar_left, &bar_top)) {
		uint32_t bar_bottom = bar_top + BOOT_SPLASH_BAR_HEIGHT;
		if (bar_bottom > top) top = bar_bottom;
	}

	if (g_display != 0 && top > g_display->height) top = g_display->height;
	return top;
}

/* Does the pixel rectangle [left, right) x [top, bottom) overlap the header? */
static bool boot_splash_rect_hits_header(uint32_t left, uint32_t right, uint32_t top, uint32_t bottom)
{
	return right > g_header_left && left < g_header_right && bottom > g_header_top && top < g_header_bottom;
}

static void boot_splash_text_clear_cell(uint32_t column, uint32_t row)
{
	uint32_t start_x = column * BOOT_SPLASH_TEXT_CELL_WIDTH;
	uint32_t start_y = row * BOOT_SPLASH_TEXT_CELL_HEIGHT;
	if (boot_splash_rect_hits_header(start_x, start_x + BOOT_SPLASH_TEXT_CELL_WIDTH, start_y, start_y + BOOT_SPLASH_TEXT_CELL_HEIGHT)) return;

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

	boot_splash_text_clear_cell(column, row);
	if (boot_splash_rect_hits_header(start_x, start_x + BOOT_SPLASH_TEXT_CELL_WIDTH, start_y, start_y + BOOT_SPLASH_TEXT_CELL_HEIGHT)) return;

	const uint8_t *glyph = &g_font8x16[(uint32_t)(uint8_t)character * FONT8X16_HEIGHT];

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
 * Copy one raw scanline, skipping the header's column range if this
 * scanline falls inside its row range -- protecting the header's own
 * pixels from ever being read as scroll source or overwritten as scroll
 * destination, without needing to repaint it afterward.
 */
static void boot_splash_copy_row(uint32_t destination_y, uint32_t source_y)
{
	uint32_t *destination = g_display->framebuffer + (uint64_t)destination_y * g_display->stride;
	uint32_t *source = g_display->framebuffer + (uint64_t)source_y * g_display->stride;
	bool masked = (destination_y >= g_header_top && destination_y < g_header_bottom) ||
		(source_y >= g_header_top && source_y < g_header_bottom);

	for (uint32_t x = 0U; x < g_display->width; x++) {
		if (masked && x >= g_header_left && x < g_header_right) continue;
		destination[x] = source[x];
	}
}

static void boot_splash_clear_row(uint32_t y)
{
	uint32_t *row = g_display->framebuffer + (uint64_t)y * g_display->stride;
	bool masked = y >= g_header_top && y < g_header_bottom;

	for (uint32_t x = 0U; x < g_display->width; x++) {
		if (masked && x >= g_header_left && x < g_header_right) continue;
		row[x] = BOOT_SPLASH_BACKGROUND;
	}
}

/*
 * Shift the whole console up by one cell height and clear the vacated last
 * row, exactly like a terminal scroll -- across the full screen, since
 * boot_splash_copy_row()/boot_splash_clear_row() already mask out the
 * header's column range on any scanline that needs it.
 */
static void boot_splash_text_scroll(void)
{
	uint32_t region_height = g_text_rows * BOOT_SPLASH_TEXT_CELL_HEIGHT;
	uint32_t cell = BOOT_SPLASH_TEXT_CELL_HEIGHT;
	if (region_height < cell) return;

	uint32_t pixel_rows = region_height - cell;
	for (uint32_t y = 0U; y < pixel_rows; y++) {
		boot_splash_copy_row(y, y + cell);
	}

	for (uint32_t y = pixel_rows; y < region_height; y++) {
		boot_splash_clear_row(y);
	}
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
static uint64_t g_text_last_present_ticks;

/*
 * Rate-limit presents to BOOT_SPLASH_TEXT_PRESENT_HZ. A burst of log lines (e.g.
 * the heap allocator debug spam, lines a fraction of a millisecond apart)
 * would otherwise fire one full-screen present per line -- far faster than
 * the host can composite -- and QEMU'''s cocoa backend visibly tears when
 * hammered like that. Content that arrives between throttled calls stays
 * marked dirty and is picked up by the next one, whether that'''s the next
 * line'''s call here or the periodic tick in boot_splash_wait().
 */
static bool boot_splash_text_flush(void)
{
	if (!g_text_dirty || g_display == 0) return true;

	uint64_t frequency = timer_get_frequency();
	if (frequency != 0ULL) {
		uint64_t interval = frequency / BOOT_SPLASH_TEXT_PRESENT_HZ;
		if (interval == 0ULL) interval = 1ULL;
		uint64_t now = timer_get_ticks();
		if (g_text_last_present_ticks != 0ULL && now - g_text_last_present_ticks < interval) {
			return true;
		}
		g_text_last_present_ticks = now;
	}

	g_text_dirty = false;

	/*
	 * Keep the bar visibly creeping forward for as long as the boot log is
	 * still reaching the screen -- "loading the OS" -- instead of it
	 * sitting frozen through the whole ipc/VFS/thread/scheduler bring-up
	 * the way it did before. Capped below BOOT_SPLASH_PROGRESS_MAX so
	 * boot_splash_finish() still has a visible amount left to close out
	 * once the log actually stops.
	 */
	if (g_visible && g_progress < BOOT_SPLASH_LOG_PROGRESS_MAX) {
		uint32_t remaining = BOOT_SPLASH_LOG_PROGRESS_MAX - g_progress;
		uint32_t step = remaining / 16U;
		g_progress += step != 0U ? step : 1U;
		(void)boot_splash_paint_bar();
	}

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
		 * Try to flush on every completed line (rate-limited inside
		 * boot_splash_text_flush()). Most of the kernel's boot log happens
		 * between boot_splash_wait() returning and boot_splash_finish()
		 * running, a long synchronous stretch with no timer-driven present of
		 * its own -- without this the scrolling log would never reach the
		 * scanout during that window. Skipped during the one-time history
		 * replay in boot_splash_show(), which has nothing to gain from
		 * checking the clock per retained line.
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

/*
 * Target number of presents for the whole replay, regardless of how much
 * history there is: boot_splash_history_line_count() below picks a lines-
 * per-present stride so a short history still gets one present per line
 * (visibly scrolling from the very first retained line) while a long one
 * gets multiple lines per present instead of a present per line -- keeping
 * the present count, and so the real-world time a present costs under
 * software-emulated QEMU (TCG), bounded independent of history size.
 */
#define BOOT_SPLASH_REPLAY_TARGET_PRESENTS 48ULL
#define BOOT_SPLASH_REPLAY_STEP_MS 70ULL
#define BOOT_SPLASH_REPLAY_MAX_MS 2500ULL

/* Cheap probe: capacity 0 reads nothing but still clamps/advances *cursor. */
static uint64_t boot_splash_history_oldest_cursor(void)
{
	uint64_t cursor = 0ULL;
	uint64_t read_size;
	char dummy;
	(void)kconsole_history_read(&cursor, &dummy, 0ULL, &read_size);
	return cursor;
}

/* Cheap forward scan (no rendering): total newline count in retained history. */
static uint64_t boot_splash_history_line_count(void)
{
	uint64_t cursor = boot_splash_history_oldest_cursor();
	uint64_t total_lines = 0ULL;
	char scan_buffer[256];
	uint64_t scan_read_size;

	for (;;) {
		if (!kconsole_history_read(&cursor, scan_buffer, sizeof(scan_buffer), &scan_read_size) || scan_read_size == 0ULL) break;
		for (uint64_t index = 0ULL; index < scan_read_size; index++) {
			if (scan_buffer[index] == '\n') total_lines++;
		}
	}

	return total_lines;
}

/*
 * Replay the retained console history from the very first line, through the
 * same scrolling text path live boot output uses, presenting every
 * lines_per_present lines with a short forced delay between presents -- so
 * the splash visibly scrolls through the whole boot log seen so far instead
 * of jumping straight to only its last screenful. g_text_replaying
 * suppresses boot_splash_text_putc()'s own per-line flush attempt so this
 * function alone controls pacing.
 *
 * Also bounded by BOOT_SPLASH_REPLAY_MAX_MS as a safety net: if presents are
 * running unusually slowly (a heavily loaded or emulated host), remaining
 * lines are drawn without presenting each one -- one final present at the
 * end catches the screen up -- so this can never turn into a real stall.
 */
static void boot_splash_replay_history(display_device_t *display)
{
	uint64_t total_lines = boot_splash_history_line_count();
	if (total_lines == 0ULL) return;

	uint64_t lines_per_present = total_lines / BOOT_SPLASH_REPLAY_TARGET_PRESENTS;
	if (lines_per_present == 0ULL) lines_per_present = 1ULL;

	uint64_t cursor = boot_splash_history_oldest_cursor();

	uint64_t frequency = timer_get_frequency();
	uint64_t step_ticks = frequency != 0ULL ? (frequency * BOOT_SPLASH_REPLAY_STEP_MS) / 1000ULL : 0ULL;
	uint64_t max_ticks = frequency != 0ULL ? (frequency * BOOT_SPLASH_REPLAY_MAX_MS) / 1000ULL : 0ULL;
	uint64_t deadline = timer_get_ticks() + max_ticks;

	char buffer[256];
	uint64_t read_size;
	bool animate = true;
	uint64_t lines_since_present = 0ULL;

	g_text_replaying = true;

	for (;;) {
		if (!kconsole_history_read(&cursor, buffer, sizeof(buffer), &read_size) || read_size == 0ULL) break;

		for (uint64_t index = 0ULL; index < read_size; index++) {
			char character = buffer[index];
			boot_splash_text_putc(character, 0);

			if (character != '\n' || !animate) continue;

			lines_since_present++;
			if (lines_since_present < lines_per_present) continue;
			lines_since_present = 0ULL;

			(void)display_present_full(display);

			if (max_ticks != 0ULL && timer_get_ticks() >= deadline) {
				animate = false;
			} else if (step_ticks != 0ULL) {
				uint64_t next = timer_get_ticks() + step_ticks;
				while (timer_get_ticks() < next) __asm__ volatile("yield");
			}
		}
	}

	if (!animate || g_text_dirty) (void)display_present_full(display);

	g_text_replaying = false;
	g_text_dirty = false;
	g_text_last_present_ticks = timer_get_ticks();
}

static void boot_splash_text_init(void)
{
	if (g_display == 0) {
		g_text_active = false;
		return;
	}

	/*
	 * The console spans the full screen in both dimensions; only the header
	 * rectangle itself (logo, widened to the progress bar's width where
	 * that's wider) is off-limits to individual cells and scroll copies --
	 * see boot_splash_rect_hits_header(). Rows above and below the header,
	 * and columns to its left and right within the header's own rows, are
	 * all fair game, so nothing about the header wastes the full row/column
	 * it happens to sit in.
	 */
	g_header_top = g_top;
	g_header_bottom = boot_splash_text_region_top();

	uint32_t scaled_width = g_boot_splash_width * g_scale;
	g_header_left = g_left;
	g_header_right = g_left + scaled_width;

	uint32_t bar_left;
	uint32_t bar_top;
	if (boot_splash_bar_rect(&bar_left, &bar_top)) {
		if (bar_left < g_header_left) g_header_left = bar_left;
		uint32_t bar_right = bar_left + BOOT_SPLASH_BAR_WIDTH;
		if (bar_right > g_header_right) g_header_right = bar_right;
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
	boot_splash_paint_logo();

	if (g_text_active) {
		/*
		 * Register for live output first (no auto-replay), then walk the
		 * retained history ourselves so it plays back with visible motion
		 * -- see boot_splash_replay_history() -- instead of appearing
		 * already fully scrolled in one instant paint.
		 */
		bool registered = kconsole_register_sink(boot_splash_text_putc, 0, false);
		if (!registered) {
			g_text_active = false;
		} else {
			boot_splash_replay_history(display);
		}
	}

	g_started_at = timer_get_ticks();
	g_progress = 80U;
	g_visible = true;
	if (!boot_splash_draw_bar()) return false;
	if (!display_present_full(display)) return false;
	kprintf("boot_splash_show: shown %ux%u at %ux scale\n", g_boot_splash_width, g_boot_splash_height, g_scale);
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
	 * By this point every line the rest of boot is ever going to print has
	 * already been drawn into the console cells (boot_splash_text_putc()
	 * runs synchronously from kputc()/kprintf()) -- but the last one or two
	 * lines may still be sitting behind the BOOT_SPLASH_TEXT_PRESENT_HZ rate
	 * gate, drawn but not yet actually presented to the screen. Force one
	 * last present here, bypassing that gate, so "loading the OS" doesn't
	 * finish a moment before its own final log lines became visible.
	 */
	if (g_text_dirty) {
		g_text_dirty = false;
		if (!display_present_full(g_display)) return false;
	}

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

	kputln("boot_splash_finish: progress complete");
	return true;
}
