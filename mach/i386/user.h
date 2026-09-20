/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/user.h
 *
 * The i386 port does not implement fork, exec or signal delivery yet. These
 * refuse cleanly so the machine-independent code, which is shared, builds
 * and reports "not supported" instead of misbehaving. See
 * <mach/machine/user.h>.
 */

#ifndef NXU_MACH_I386_USER_H
#define NXU_MACH_I386_USER_H

#include <mach/i386/thread.h>

#include <stdbool.h>
#include <stdint.h>

#define MACHINE_USER_CONTEXT 0

static inline bool machine_user_fork_state(machine_thread_t *child)
{
	(void)child;
	return false;
}

static inline bool machine_user_exec_state(uint64_t entry, uint64_t stack, uint64_t argc, uint64_t argv)
{
	(void)entry;
	(void)stack;
	(void)argc;
	(void)argv;
	return false;
}

static inline bool machine_user_signal_push(uint64_t handler, uint64_t restorer, uint32_t signal, uint32_t saved_mask)
{
	(void)handler;
	(void)restorer;
	(void)signal;
	(void)saved_mask;
	return false;
}

static inline bool machine_user_signal_pop(uint32_t *saved_mask)
{
	(void)saved_mask;
	return false;
}

#endif
