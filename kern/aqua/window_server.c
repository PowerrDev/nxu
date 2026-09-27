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

#include <skylight/skylight.h>

#include <stdbool.h>
#include <stdint.h>

#if defined(NXU_WINDOWSERVER)
#include <WindowServer/WSPrivate.h>

/*
 * WindowServer composes into the primary Skylight display's scanout surface
 * and presents through Skylight: which windows go where is WindowServer's,
 * getting the pixels to the device is Skylight's and its backend's.
 */
static bool
ws_display_present(
	void *context,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height
)
{
	return sl_present((sl_display_t *)context, (sl_rect_t) { (int32_t)x, (int32_t)y, width, height });
}

bool
windowserver_bootstrap(void)
{
	sl_display_t *display = sl_display_primary();
	if (display == 0) return false;

	sl_surface_t *surface = sl_display_surface(display);
	uint32_t stride = 0U;
	uint32_t *pixels = sl_surface_map(surface, &stride);
	sl_surface_unmap(surface);
	if (pixels == 0 || sl_surface_width(surface) == 0U || sl_surface_height(surface) == 0U) return false;

	WSDisplay ws_display = {
		.framebuffer = pixels,
		.width = sl_surface_width(surface),
		.height = sl_surface_height(surface),
		.stride = stride,
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

