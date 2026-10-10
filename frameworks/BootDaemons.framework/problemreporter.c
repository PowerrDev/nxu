#include <nxu/syscall.h>
#include <nxu/string.h>
#include <reporter_fonts.h>
#include <stdbool.h>
#include <stdint.h>

#define WIDTH 520U
#define HEIGHT 220U
static nxu_crash_notification_t g_notice;
static char g_details[8192];
static uint32_t *g_pixels;
static bool g_show_details;
static uint32_t g_line;

static void copy(char *out, uint32_t capacity, const char *in)
{
	uint32_t n = 0;

	while (*in && n + 1 < capacity)
		out[n++] = *in++;
	out[n] = 0;
}

static void rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
	for (uint32_t row = y; row < HEIGHT && row < y + h; row++)
		for (uint32_t col = x; col < WIDTH && col < x + w; col++)
			g_pixels[row * WIDTH + col] = color;
}

static const reporter_glyph_t *glyph(const reporter_font_t *font, unsigned char c)
{
	return &font->glyphs[(c >= 32 && c <= 126 ? c : '?') - 32];
}

static uint32_t text_width(const char *s, const reporter_font_t *font)
{
	uint32_t width = 0;

	while (*s)
		width += glyph(font, (unsigned char)*s++)->advance;
	return width;
}

static void draw_text(uint32_t x, uint32_t y, const char *s, const reporter_font_t *font, uint32_t color)
{
	uint32_t start = x;

	while (*s && y + font->height < HEIGHT - 58) {
		unsigned char c = (unsigned char)*s++;

		const reporter_glyph_t *g = glyph(font, c);

		if (c == '\n' || x + g->advance > WIDTH - 28) {
			y += font->height + 3;
			x = start;
			if (c == '\n')
				continue;

			if (y + font->height >= HEIGHT - 58)
				break;
		}

		for (uint32_t row = 0; row < g->height; row++)
			for (uint32_t col = 0; col < g->width; col++) {
				int32_t px = (int32_t)x + g->x + (int32_t)col;
				int32_t py = (int32_t)y + g->y + (int32_t)row;

				if (px < 0 || py < 0 || px >= (int32_t)WIDTH || py >= (int32_t)HEIGHT)
					continue;

				uint32_t alpha = font->pixels[g->offset + row * g->width + col];
				uint32_t old = g_pixels[py * WIDTH + px], blended = 0xff000000U;

				for (uint32_t shift = 0; shift < 24; shift += 8)
					blended |= ((((color >> shift) & 255) * alpha + ((old >> shift) & 255) * (255 - alpha) + 127) / 255)
							   << shift;
				g_pixels[py * WIDTH + px] = blended;
			}

		x += g->advance;
	}
}

static void button(uint32_t x, uint32_t width, const char *label, bool primary)
{
	uint32_t y = HEIGHT - 50, height = 28, radius = 5;

	for (uint32_t row = 0; row < height; row++)
		for (uint32_t col = 0; col < width; col++) {
			int32_t dx = col < radius			 ? (int32_t)radius - (int32_t)col
						 : col >= width - radius ? (int32_t)col - (int32_t)(width - radius - 1)
												 : 0;

			int32_t dy = row < radius			  ? (int32_t)radius - (int32_t)row
						 : row >= height - radius ? (int32_t)row - (int32_t)(height - radius - 1)
												  : 0;
			if (dx * dx + dy * dy > (int32_t)(radius * radius))
				continue;

			bool border = row == 0 || row == height - 1 || col == 0 || col == width - 1 || dx * dx + dy * dy > 16;
			uint32_t color = primary ? (border ? 0xff0870d9U : 0xff1686eeU) : (border ? 0xffc8c8ccU : 0xfffdfdfdU);
			g_pixels[(y + row) * WIDTH + x + col] = color;
		}

	/* Center the Inter label in the button. */
	uint32_t label_width = text_width(label, &regular);
	uint32_t left = x + (width - label_width) / 2;
	uint32_t label_y = y + 4;
	// Render the same antialiased Inter glyphs without body-text clipping.
	for (const char *s = label; *s; s++) {
		const reporter_glyph_t *g = glyph(&regular, (unsigned char)*s);

		for (uint32_t row = 0; row < g->height; row++)
			for (uint32_t col = 0; col < g->width; col++) {
				uint32_t at = (label_y + g->y + row) * WIDTH + left + g->x + col;
				uint32_t alpha = regular.pixels[g->offset + row * g->width + col];
				uint32_t old = g_pixels[at], color = primary ? 0xffffffffU : 0xff202024U, blended = 0xff000000U;

				for (uint32_t shift = 0; shift < 24; shift += 8)
					blended |= ((((color >> shift) & 255) * alpha + ((old >> shift) & 255) * (255 - alpha) + 127) / 255)
							   << shift;
				g_pixels[at] = blended;
			}

		left += g->advance;
	}
}

static bool bundle_path(char *out)
{
	copy(out, NXU_CRASH_PATH_MAX, g_notice.executable);
	for (uint32_t i = 0; out[i]; i++)
		if (out[i] == '.' && out[i + 1] == 'a' && out[i + 2] == 'p' && out[i + 3] == 'p' && out[i + 4] == '/') {
			out[i + 4] = 0;
			return true;
		}

	return false;
}

static bool draw(uint32_t connection)
{
	rect(0, 0, WIDTH, HEIGHT, 0xffefeff1U);
	if (g_show_details) {
		rect(16, 12, WIDTH - 32, HEIGHT - 76, 0xffffffffU);

		const char *s = g_details;

		for (uint32_t n = 0; n < g_line && *s;)
			if (*s++ == '\n')
				n++;
		draw_text(24, 18, s, &regular, 0xff202024U);
	} else {
		char heading[96];
		copy(heading, sizeof(heading), g_notice.name);

		uint32_t n = nxu_strlen(heading);
		copy(heading + n, sizeof(heading) - n, " quit unexpectedly.");
		draw_text(28, 24, heading, &semibold, 0xff202024U);
		draw_text(28, 62,
				  "Click Report to see detailed information about the crash.\nThe diagnostic report was saved on this "
				  "computer.",
				  &regular, 0xff303034U);
	}

	char bundle[NXU_CRASH_PATH_MAX];
	bool reopen = bundle_path(bundle);
	button(reopen ? 172 : 280, 96, "Ignore", false);
	button(reopen ? 280 : 388, 104, g_show_details ? "Back" : "Report...", !reopen);
	if (reopen)
		button(396, 96, "Reopen", true);

	nxu_ui_submit_t submit = {
		.pixels = (uint64_t)(uintptr_t)g_pixels, .width = WIDTH, .height = HEIGHT, .stride = WIDTH};
	return nxu_ui_submit(connection, &submit) >= 0;
}

int main(void)
{
	int64_t memory = nxu_mmap((uint64_t)WIDTH * HEIGHT * 4, 0);

	if (memory < 0)
		return 1;

	g_pixels = (uint32_t *)(uintptr_t)memory;
	for (;;) {
		int64_t result = nxu_crash(NXU_CRASH_UI_NEXT, (uint64_t)(uintptr_t)&g_notice);

		if (result == -NXU_SYS_E_INTERRUPTED)
			continue;

		if (result < 0)
			return 2;

		int64_t child = nxu_fork();

		if (child < 0)
			continue;

		if (child > 0) {
			uint64_t status;

			while (nxu_wait((uint64_t)child, &status) == -NXU_SYS_E_INTERRUPTED) {
			}

			continue;
		}

		/* One short-lived presenter per dialog lets desktop connection cleanup
		 * follow the normal app exit path, and keeps the listener off UI calls. */
		nxu_ui_connect_t info = {.struct_size = sizeof(info),
								 .kind = NXU_UI_KIND_APP,
								 .width = WIDTH,
								 .height = HEIGHT + 32,
								 .min_width = WIDTH,
								 .min_height = HEIGHT + 32,
								 .max_width = WIDTH,
								 .max_height = HEIGHT + 32,
								 .titlebar_height = 32,
								 .corner_radius = 8};
		copy(info.name, sizeof(info.name), "Problem Reporter");

		int64_t connection = nxu_ui_connect(&info);

		if (connection < 0)
			return 3;

		g_show_details = false;
		g_line = 0;
		g_details[0] = 0;
		if (!draw((uint32_t)connection))
			return 4;

		const char *ready = "problemreporter: dialog ready\n";
		(void)nxu_write(1, ready, nxu_strlen(ready));
		for (;;) {
			nxu_ui_message_t message;
			result = nxu_ui_receive((uint32_t)connection, &message, NXU_UI_WAIT_FOREVER);
			if (result < 0 || message.type == NXU_UI_MSG_QUIT)
				break;

			if (message.type == NXU_UI_MSG_EVENT && message.event == NXU_UI_EVENT_POINTER_DOWN &&
				message.y >= (int32_t)HEIGHT - 50 && message.y < (int32_t)HEIGHT - 22) {
				char button_bundle[NXU_CRASH_PATH_MAX];
				bool can_reopen = bundle_path(button_bundle);
				int32_t ignore_x = can_reopen ? 172 : 280;
				int32_t report_x = can_reopen ? 280 : 388;

				if (message.x >= ignore_x && message.x < ignore_x + 96)
					break;

				if (can_reopen && message.x >= 396 && message.x < 492) {
					char bundle[NXU_CRASH_PATH_MAX];

					if (bundle_path(bundle)) {
						if (nxu_ui_control(NXU_UI_CONTROL_LAUNCH, (uint64_t)(uintptr_t)bundle) >= 0)
							break;
					}
				}

				if (message.x >= report_x && message.x < report_x + 104) {
					g_show_details = !g_show_details;
					g_line = 0;

					int64_t fd = nxu_open(g_notice.report, NXU_O_READ);

					if (fd >= 0) {
						int64_t n = nxu_read((uint64_t)fd, g_details, sizeof(g_details) - 1);
						g_details[n > 0 ? (uint32_t)n : 0U] = 0;
						(void)nxu_close((uint64_t)fd);
					} else
						copy(g_details, sizeof(g_details), "Unable to read the saved report.");
				}
			}

			if (g_show_details && message.type == NXU_UI_MSG_EVENT && message.event == NXU_UI_EVENT_SCROLL) {
				if (message.delta < 0 && g_line < 300)
					g_line += 3;
				else if (message.delta > 0)
					g_line = g_line >= 3 ? g_line - 3 : 0;
			}

			draw((uint32_t)connection);
		}

		nxu_ui_submit_t close = {.action = NXU_UI_ACTION_CLOSE};
		(void)nxu_ui_submit((uint32_t)connection, &close);
		return 0;
	}
}
