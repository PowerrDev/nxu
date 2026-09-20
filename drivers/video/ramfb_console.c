#include <drivers/video/ramfb_console.h>

#include <kern/arm64/timer.h>
#include <kern/console/console.h>
#include <kern/console/font8x16.h>
#include <platform/fw_cfg.h>
#include <vm/pmm.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define RAMFB_WIDTH 1280U
#define RAMFB_HEIGHT 800U
#define RAMFB_BYTES_PER_PIXEL 4U
#define RAMFB_STRIDE (RAMFB_WIDTH * RAMFB_BYTES_PER_PIXEL)

#define RAMFB_FONT_WIDTH FONT8X16_WIDTH
#define RAMFB_FONT_HEIGHT FONT8X16_HEIGHT
#define RAMFB_FONT_SCALE 1U
#define RAMFB_CELL_WIDTH 8U
#define RAMFB_CELL_HEIGHT 18U

#define RAMFB_FOREGROUND 0xFFFFFFFFU
#define RAMFB_BACKGROUND 0x00000000U
#define RAMFB_LINE_DELAY_MS 0ULL

#define RAMFB_FORMAT_XRGB8888 0x34325258U
#define RAMFB_FILE_NAME "etc/ramfb"

typedef struct __attribute__((packed)) {
	uint64_t address;
	uint32_t fourcc;
	uint32_t flags;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
} ramfb_config_t;

_Static_assert(sizeof(ramfb_config_t) == 28U, "ramfb config must match QEMU ABI");

typedef struct {
	uint32_t *framebuffer;
	uint64_t framebuffer_physical;
	uint64_t framebuffer_pages;
	uint32_t cursor_x;
	uint32_t cursor_y;
	uint32_t columns;
	uint32_t rows;
	bool active;
	bool sink_registered;
} ramfb_console_t;

static ramfb_console_t g_ramfb_console;

static uint32_t ramfb_to_be32(uint32_t value)
{
	return ((value & 0x000000FFU) << 24U)
		| ((value & 0x0000FF00U) << 8U)
		| ((value & 0x00FF0000U) >> 8U)
		| ((value & 0xFF000000U) >> 24U);
}

static uint64_t ramfb_to_be64(uint64_t value)
{
	uint64_t low = ramfb_to_be32((uint32_t)value);
	uint64_t high = ramfb_to_be32((uint32_t)(value >> 32U));
	return (low << 32U) | high;
}

static void ramfb_clear(void)
{
	uint64_t pixels = (uint64_t)RAMFB_WIDTH * RAMFB_HEIGHT;
	for (uint64_t index = 0ULL; index < pixels; index++) {
		g_ramfb_console.framebuffer[index] = RAMFB_BACKGROUND;
	}
}

static void ramfb_clear_cell(uint32_t cell_x, uint32_t cell_y)
{
	uint32_t start_x = cell_x * RAMFB_CELL_WIDTH;
	uint32_t start_y = cell_y * RAMFB_CELL_HEIGHT;

	for (uint32_t y = 0U; y < RAMFB_CELL_HEIGHT; y++) {
		uint32_t *row = g_ramfb_console.framebuffer
			+ (uint64_t)(start_y + y) * RAMFB_WIDTH
			+ start_x;

		for (uint32_t x = 0U; x < RAMFB_CELL_WIDTH; x++) {
			row[x] = RAMFB_BACKGROUND;
		}
	}
}

static void ramfb_draw_character(char character)
{
	if (character < 32 || character > 126) character = '?';

	uint32_t cell_x = g_ramfb_console.cursor_x;
	uint32_t cell_y = g_ramfb_console.cursor_y;
	uint32_t start_x = cell_x * RAMFB_CELL_WIDTH + 1U;
	uint32_t start_y = cell_y * RAMFB_CELL_HEIGHT + 1U;
	const uint8_t *glyph = &g_font8x16[(uint32_t)(uint8_t)character * RAMFB_FONT_HEIGHT];

	ramfb_clear_cell(cell_x, cell_y);

	for (uint32_t row = 0U; row < RAMFB_FONT_HEIGHT; row++) {
		uint8_t bits = glyph[row];

		for (uint32_t column = 0U; column < RAMFB_FONT_WIDTH; column++) {
			if ((bits & (1U << column)) == 0U) continue;
			
			uint32_t pixel_x = start_x + column * RAMFB_FONT_SCALE;
			uint32_t pixel_y = start_y + row * RAMFB_FONT_SCALE;

			for (uint32_t scale_y = 0U; scale_y < RAMFB_FONT_SCALE; scale_y++) {
				uint32_t *pixels = g_ramfb_console.framebuffer
					+ (uint64_t)(pixel_y + scale_y) * RAMFB_WIDTH
					+ pixel_x;

				for (uint32_t scale_x = 0U; scale_x < RAMFB_FONT_SCALE; scale_x++) {
					pixels[scale_x] = RAMFB_FOREGROUND;
				}
			}
		}
	}
}

static void ramfb_scroll(void)
{
	uint32_t pixel_rows = RAMFB_HEIGHT - RAMFB_CELL_HEIGHT;

	for (uint32_t y = 0U; y < pixel_rows; y++) {
		uint32_t *destination = g_ramfb_console.framebuffer
			+ (uint64_t)y * RAMFB_WIDTH;
		uint32_t *source = g_ramfb_console.framebuffer
			+ (uint64_t)(y + RAMFB_CELL_HEIGHT) * RAMFB_WIDTH;

		for (uint32_t x = 0U; x < RAMFB_WIDTH; x++) {
			destination[x] = source[x];
		}
	}

	for (uint32_t y = pixel_rows; y < RAMFB_HEIGHT; y++) {
		uint32_t *row = g_ramfb_console.framebuffer
			+ (uint64_t)y * RAMFB_WIDTH;

		for (uint32_t x = 0U; x < RAMFB_WIDTH; x++) {
			row[x] = RAMFB_BACKGROUND;
		}
	}
}

static void ramfb_advance_line(bool delay)
{
	g_ramfb_console.cursor_x = 0U;
	g_ramfb_console.cursor_y++;

	if (g_ramfb_console.cursor_y >= g_ramfb_console.rows) {
		ramfb_scroll();
		g_ramfb_console.cursor_y = g_ramfb_console.rows - 1U;
	}

	if (delay) timer_delay_ms(RAMFB_LINE_DELAY_MS);
}

static void ramfb_console_sink(char character, void *context)
{
	(void)context;
	if (!g_ramfb_console.active) return;

	if (character == '\r') {
		g_ramfb_console.cursor_x = 0U;
		return;
	}

	if (character == '\n') {
		ramfb_advance_line(true);
		return;
	}

	if (character == '\t') {
		uint32_t spaces = 4U - (g_ramfb_console.cursor_x % 4U);
		for (uint32_t index = 0U; index < spaces; index++) {
			ramfb_console_sink(' ', context);
		}
		return;
	}

	if (character == '\b') {
		if (g_ramfb_console.cursor_x != 0U) {
			g_ramfb_console.cursor_x--;
			ramfb_clear_cell(
				g_ramfb_console.cursor_x,
				g_ramfb_console.cursor_y
			);
		}
		return;
	}

	if (g_ramfb_console.cursor_x >= g_ramfb_console.columns) {
		ramfb_advance_line(false);
	}

	ramfb_draw_character(character);
	g_ramfb_console.cursor_x++;
}

bool ramfb_console_init(const platform_t *platform)
{
	if (g_ramfb_console.active) return true;

	if (g_ramfb_console.framebuffer != 0) return false;
	if (platform == 0 || platform->fw_cfg.size == 0ULL) return false;

	fw_cfg_file_t ramfb_file;
	if (!fw_cfg_find_file(&platform->fw_cfg, RAMFB_FILE_NAME, &ramfb_file)) {
		return false;
	}

	if (ramfb_file.size != sizeof(ramfb_config_t)) return false;

	uint64_t framebuffer_bytes = (uint64_t)RAMFB_STRIDE * RAMFB_HEIGHT;
	uint64_t framebuffer_pages = (framebuffer_bytes + PMM_PAGE_SIZE - 1ULL) / PMM_PAGE_SIZE;
	uint64_t framebuffer_physical;

	if (!pmm_allocate_contiguous_pages(
		framebuffer_pages,
		&framebuffer_physical
	)) {
		return false;
	}

	uint64_t framebuffer_virtual;
	if (!vmm_physical_to_higher_half(
		framebuffer_physical,
		&framebuffer_virtual
	)) {
		(void)pmm_free_contiguous_pages(
			framebuffer_physical,
			framebuffer_pages
		);
		return false;
	}

	g_ramfb_console.framebuffer = (uint32_t *)framebuffer_virtual;
	g_ramfb_console.framebuffer_physical = framebuffer_physical;
	g_ramfb_console.framebuffer_pages = framebuffer_pages;
	g_ramfb_console.cursor_x = 0U;
	g_ramfb_console.cursor_y = 0U;
	g_ramfb_console.columns = RAMFB_WIDTH / RAMFB_CELL_WIDTH;
	g_ramfb_console.rows = RAMFB_HEIGHT / RAMFB_CELL_HEIGHT;

	ramfb_clear();

	ramfb_config_t config = {
		.address = ramfb_to_be64(framebuffer_physical),
		.fourcc = ramfb_to_be32(RAMFB_FORMAT_XRGB8888),
		.flags = ramfb_to_be32(0U),
		.width = ramfb_to_be32(RAMFB_WIDTH),
		.height = ramfb_to_be32(RAMFB_HEIGHT),
		.stride = ramfb_to_be32(RAMFB_STRIDE)
	};

	if (!fw_cfg_dma_write(
		&platform->fw_cfg,
		ramfb_file.selector,
		&config,
		sizeof(config)
	)) {
		(void)pmm_free_contiguous_pages(
			framebuffer_physical,
			framebuffer_pages
		);
		memset(&g_ramfb_console, 0, sizeof(g_ramfb_console));
		return false;
	}

	g_ramfb_console.active = true;

	/*
	 * Do not replay the UART history into RAMFB. Repainting hundreds of old
	 * boot lines here can spend seconds scrolling a 1280x800 framebuffer and
	 * makes the graphical handoff look far slower than the kernel actually is.
	 */
	if (!kconsole_register_sink(ramfb_console_sink, 0, false)) {
		/*
		 * QEMU already owns a live mapping of this RAM. Keep the backing
		 * pages reserved rather than returning them to PMM underneath ramfb.
		 */
		g_ramfb_console.active = false;
		return false;
	}

	g_ramfb_console.sink_registered = true;
	return true;
}

/*
 * Enable or disable console mirroring without relinquishing the RAMFB scanout.
 * UI disables mirroring while it owns the visible desktop so UART logging can
 * remain extremely verbose without scribbling directly into live scanout.
 */
bool ramfb_console_set_mirroring(bool enabled)
{
	if (!g_ramfb_console.active) return false;
	if (enabled == g_ramfb_console.sink_registered) return true;

	if (enabled) {
		if (!kconsole_register_sink(ramfb_console_sink, 0, false)) return false;
		g_ramfb_console.sink_registered = true;
		return true;
	}

	if (!kconsole_unregister_sink(ramfb_console_sink, 0)) return false;
	g_ramfb_console.sink_registered = false;
	return true;
}

bool ramfb_console_available(void)
{
	return g_ramfb_console.active;
}

uint32_t *ramfb_console_framebuffer(void)
{
	return g_ramfb_console.active ? g_ramfb_console.framebuffer : 0;
}

uint32_t ramfb_console_width(void)
{
	return g_ramfb_console.active ? RAMFB_WIDTH : 0U;
}

uint32_t ramfb_console_height(void)
{
	return g_ramfb_console.active ? RAMFB_HEIGHT : 0U;
}

uint32_t ramfb_console_stride(void)
{
	return g_ramfb_console.active ? RAMFB_WIDTH : 0U;
}
