/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/ipc_init.c
 *
 * Functions to initialize the NXPC kernel IPC system.
 */

#include <kern/ipc/ipc_init.h>

#include <kern/console/console.h>
#include <kern/ipc/ipc_kmsg.h>
#include <kern/ipc/ipc_port.h>
#include <kern/ipc/ipc_types.h>

#include <stdbool.h>

static bool g_ipc_initialized;

/*
 * Routine:     ipc_init
 * Purpose:
 *              Initialize the kernel-owned message and port layers used by
 *              NXPC before per-task names and userspace rights are introduced.
 */
void
ipc_init(void)
{
	if (g_ipc_initialized) return;

	ipc_kmsg_init();
	ipc_port_init();
	g_ipc_initialized = true;

	kprintf(NXPC_LOG_PREFIX "transport online, inline limit %u bytes, queue limit %u\n", IPC_KMSG_MAX_INLINE_SIZE, IPC_PORT_QUEUE_LIMIT);
}

bool
ipc_initialized(void)
{
	return g_ipc_initialized;
}
