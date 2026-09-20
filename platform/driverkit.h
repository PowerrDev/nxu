/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/driverkit.h
 *
 * DriverKit: the single call that brings up every device driver the kernel
 * ships, in dependency order -- interrupt controller, input, boot mode,
 * block, display, RTC, then the VirtIO bus scan that attaches the real
 * devices. Boot-args decide which families are probed at all.
 *
 * It runs right after the kernel heap comes up and before IPC, processes
 * and the filesystem, so the boot splash can reach the screen the moment a
 * display exists rather than after the whole boot sequence.
 */

#ifndef NXU_PLATFORM_DRIVERKIT_H
#define NXU_PLATFORM_DRIVERKIT_H

#include <stdbool.h>

/*
 * Which driver families boot-args left enabled. Filled in by
 * driverkit_init() so the rest of boot does not have to ask again.
 */
typedef struct {
	bool input_enabled;
	bool block_enabled;
	bool gpu_enabled;
} driverkit_config_t;

/*
 * driverkit_init
 *
 * Bring up every driver family. Returns false, after logging which family
 * failed, if a required one could not start; boot cannot continue then.
 */
bool driverkit_init(driverkit_config_t *config);

/*
 * driverkit_service_boot_input
 *
 * Poll recovery-startup input while the splash owns the scanout. IRQ
 * delivery is not required: the VirtIO input service drains the transport
 * directly. Passed to boot_splash_wait() as its service hook.
 */
void driverkit_service_boot_input(void);

#endif
