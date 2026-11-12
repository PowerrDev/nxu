/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/user.h
 *
 * The i386 half of <kern/machine/user.h>. The port delivers signals: it
 * builds and restores handler frames (kern/i386/user.c) and takes pending
 * signals on every return to ring 3 (kern/i386/syscall_trap.c). It does not
 * implement fork or exec, which refuse cleanly here so the shared
 * machine-independent code builds and reports "not supported".
 */

#ifndef NXU_KERN_I386_USER_H
#define NXU_KERN_I386_USER_H

#include <kern/i386/thread.h>

#include <stdbool.h>
#include <stdint.h>

#define MACHINE_USER_CONTEXT 0
#define MACHINE_USER_SIGNALS 1

static inline bool
machine_user_fork_state(machine_thread_t *child)
{
	(void)child;
	return false;
}

static inline bool
machine_user_exec_state(uint64_t entry, uint64_t stack, uint64_t argc, uint64_t argv)
{
	(void)entry;
	(void)stack;
	(void)argc;
	(void)argv;
	return false;
}

bool machine_user_signal_push(uint64_t handler, uint64_t restorer, uint32_t signal, uint32_t saved_mask);
bool machine_user_signal_pop(uint32_t *saved_mask);

#endif
