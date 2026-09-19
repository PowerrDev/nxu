/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/console/display_owner.c
 */

#include <kern/console/display_owner.h>

static proc_id_t g_display_owner_pid = PROC_PID_KERNEL;

bool
display_owner_claim(proc_id_t pid)
{
	if (pid == PROC_PID_KERNEL) return false;

	if (g_display_owner_pid != PROC_PID_KERNEL) {
		if (g_display_owner_pid == pid) return true;
		if (proc_find(g_display_owner_pid) != 0) return false;
	}

	g_display_owner_pid = pid;
	return true;
}

bool
display_owner_is(proc_id_t pid)
{
	return pid != PROC_PID_KERNEL && g_display_owner_pid == pid;
}
