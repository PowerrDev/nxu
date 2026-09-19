/**
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */

/**
 * File:        kern/aqua/window_server.c
 *
 * NXU platform bootstrap for WindowServer.framework.
 *
 * WindowServer owns window management and composition after initialization.
 * Aqua/UIService owns application rendering and input policy.
 */

#include <kern/aqua/window_server.h>

#include <drivers/video/display.h>

#include <stdbool.h>
#include <stdint.h>

#if defined(NXU_WINDOWSERVER)
#include <WindowServer/WSPrivate.h>

static bool
ws_display_present(
	void *context,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height
)
{
	return display_present((display_device_t *)context, x, y, width, height);
}

bool
windowserver_bootstrap(void)
{
	display_device_t *display = display_primary();
	if (display == 0 ||
		display->framebuffer == 0 ||
		display->width == 0U ||
		display->height == 0U ||
		display->stride < display->width ||
		display->bytes_per_pixel != 4U) {
		return false;
	}

	WSDisplay ws_display = {
		.framebuffer = (uint32_t *)display->framebuffer,
		.width = display->width,
		.height = display->height,
		.stride = display->stride,
		.present_context = display,
		.present = ws_display_present,
	};

	return WS_Initialize(ws_display);
}
#else
bool
windowserver_bootstrap(void)
{
	return false;
}
#endif
