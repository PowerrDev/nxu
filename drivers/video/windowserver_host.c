/**
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */

#include <drivers/video/windowserver_host.h>
#include <drivers/video/display.h>
#include <WindowServerNXU.h>

bool
windowserver_bootstrap(void)
{
#if !defined(NXU_WINDOWSERVER)
    return false;
#else
    display_device_t *display = display_primary();

    if (display == 0 || display->framebuffer == 0 ||
        display->width == 0U || display->height == 0U ||
        display->stride < display->width || display->bytes_per_pixel != 4U) {
        return false;
    }

    if (!windowserver_nxu_bootstrap(
            (uint32_t *)display->framebuffer,
            display->width,
            display->height,
            display->stride)) {
        return false;
    }

    return display_present_full(display);
#endif
}
