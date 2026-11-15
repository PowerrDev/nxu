/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/video/ui_service_login.h
 *
 * What the UIService hosts (platform/{arm64,i386}/services/ui_service.c)
 * share for the login screen: keyboard events, and the passcode callbacks
 * behind UIServiceRunLogin, answered by the kernel's passcode store
 * (kern/security/passcode.h).
 *
 * The desktop is always locked. Nothing but the store accepting a passcode
 * (or setting the first one) unlocks it: a store that cannot answer yet
 * (the system volume not mounted) leaves the login screen waiting, never
 * the desktop open. Only a test kernel built with NXU_LOGIN_UNLOCKED skips
 * the login screen.
 */

#ifndef NXU_DRIVERS_VIDEO_UI_SERVICE_LOGIN_H
#define NXU_DRIVERS_VIDEO_UI_SERVICE_LOGIN_H

#if defined(NXU_UI_SERVICE)

#include <UIService.h>

#include <stdbool.h>
#include <stdint.h>

/* UI_SERVICE_HOST_CAP_KEYBOARD when a keyboard is attached, else 0. */
uint64_t ui_service_keyboard_capability(void);

/* The next queued key press as a UI_SERVICE_EVENT_KEY_DOWN, if there is one. */
bool ui_service_poll_key(UIServiceHostEvent *event);

/*
 * Before the desktop app starts: true when it may. The login screen runs
 * until the passcode store accepts a passcode, and anything else keeps the
 * desktop locked (false).
 */
bool ui_service_login(const UIServiceHostV5 *host);

#endif

#endif
