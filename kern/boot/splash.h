/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/boot/splash.h
 *
 * Declares the kernel boot splash interfaces and data structures.
 */
#ifndef NXU_KERN_BOOT_SPLASH_H
#define NXU_KERN_BOOT_SPLASH_H

#include <drivers/video/display.h>

#include <stdbool.h>
#include <stdint.h>

typedef void (*boot_splash_service_t)(void);

bool boot_splash_show(display_device_t *display);
bool boot_splash_set_status(const char *status);
bool boot_splash_wait(uint64_t milliseconds, boot_splash_service_t service);
bool boot_splash_finish(void);

#endif
