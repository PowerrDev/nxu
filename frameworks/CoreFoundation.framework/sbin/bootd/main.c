/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/CoreFoundation.framework/sbin/bootd/main.c
 *
 * PID 1 entry point. bootd owns root service discovery and process lifetime;
 * parsers and configuration translation remain reusable support libraries.
 */

#include <frameworks/CoreFoundation.framework/sbin/bootd/manager.h>

#include <nxu/string.h>
#include <nxu/syscall.h>

static bootd_manager_t g_bootd_manager;

static void
bootd_log(const char *message)
{
	(void)nxu_write(1U, message, nxu_strlen(message));
}

int
main(void)
{
	if (nxu_getpid() != 1) {
		bootd_log("bootd: refusing to run outside PID 1\n");
		return 1;
	}

	bootd_log("bootd: userspace service manager started as PID 1\n");
	if (!bootd_manager_init(&g_bootd_manager)) {
		bootd_log("bootd: service discovery failed\n");
		return 2;
	}

	bootd_manager_start(&g_bootd_manager);

	for (;;) {
		bootd_manager_poll(&g_bootd_manager);
		(void)nxu_yield();
	}
}
