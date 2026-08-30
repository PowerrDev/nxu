#include <drivers/video/display.h>

#include <kern/console/console.h>

#include <stdbool.h>
#include <stdint.h>

static display_device_t *g_primary_display;
static bool g_display_initialized;

bool display_init(void)
{
	if (g_display_initialized) return true;
	g_primary_display = 0;
	g_display_initialized = true;
	return true;
}

bool display_register(display_device_t *display)
{
	if (
		!g_display_initialized ||
		display == 0 ||
		display->registered ||
		display->framebuffer == 0 ||
		display->width == 0U ||
		display->height == 0U ||
		display->stride < display->width ||
		display->bytes_per_pixel == 0U ||
		display->present == 0 ||
		g_primary_display != 0
	) {
		return false;
	}

	display->registered = true;
	g_primary_display = display;
	return true;
}

display_device_t *display_primary(void)
{
	return g_primary_display;
}

bool display_present(
	display_device_t *display,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height
)
{
	if (
		display == 0 ||
		!display->registered ||
		width == 0U ||
		height == 0U ||
		x >= display->width ||
		y >= display->height ||
		width > display->width - x ||
		height > display->height - y
	) {
		return false;
	}

	return display->present(display, x, y, width, height);
}

bool display_present_full(display_device_t *display)
{
	if (display == 0) return false;
	return display_present(display, 0U, 0U, display->width, display->height);
}

void display_dump(void)
{
	if (g_primary_display == 0) {
		kputln("NXUDisplayDriverFamily: no primary display");
		return;
	}

	kprintf(
		"NXUDisplayDriverFamily: primary %ux%u, stride %u pixels, %u Bpp\n",
		g_primary_display->width,
		g_primary_display->height,
		g_primary_display->stride,
		g_primary_display->bytes_per_pixel
	);
}
