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

#endif
