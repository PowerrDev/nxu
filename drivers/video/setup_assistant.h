/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/video/setup_assistant.h
 *
 * The first-boot setup assistant, after macOS's: "hello" written by hand,
 * then a card that asks for an account, a device name, the Dock, the menu
 * bar clock and a wallpaper, and mainOS saying hello back.
 */
#ifndef NXU_DRIVERS_VIDEO_SETUP_ASSISTANT_H
#define NXU_DRIVERS_VIDEO_SETUP_ASSISTANT_H

#include <drivers/video/display.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Run the assistant on display, over wallpaper (the screen's picture, one
 * display->width wide row after another, or null for a flat colour; the
 * wallpaper chosen in it is drawn into this buffer). True once the password
 * is set in tepOS and the choices are written: the session is unlocked.
 */
bool setup_assistant_run(display_device_t *display, uint32_t *wallpaper, uint32_t scale_permille);

/*
 * Draw the Dynamic Island at rest at the top of display, as the desktop
 * shows it: for the lock screen (the assistant draws its own).
 */
void setup_assistant_draw_island(display_device_t *display, uint32_t scale_permille);

/*
 * The desktop shows a one-time welcome in the Dynamic Island after setup;
 * every later boot removes the note that asks for it. True once that is
 * done (or there was nothing to do), false while the system volume is not
 * mounted yet.
 */
bool setup_assistant_forget_welcome(void);

#endif
