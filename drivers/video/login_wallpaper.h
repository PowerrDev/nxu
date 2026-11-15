/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/video/login_wallpaper.h
 *
 * Interface for drawing the embedded login wallpaper.
 */
#ifndef NXU_DRIVERS_VIDEO_LOGIN_WALLPAPER_H
#define NXU_DRIVERS_VIDEO_LOGIN_WALLPAPER_H

#include <drivers/video/display.h>

bool login_wallpaper_draw(display_device_t *display);

/*
 * login_wallpaper_draw_qoi:
 *
 * Decode the QOI image in [start, end) (RGB, at most 1920x1080) and draw it
 * over the whole of display, scaled to cover it. Returns false, with the
 * framebuffer possibly part drawn, when the image is not one it takes.
 */
bool login_wallpaper_draw_qoi(display_device_t *display, const uint8_t *start, const uint8_t *end);

#endif
