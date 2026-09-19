/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/machine/thread.h
 *
 * Architecture dispatch for the machine-dependent half of a thread, in the
 * manner of XNU's <machine/thread.h>. Machine-independent code (kern/process,
 * kern/sched_prism) includes this header and uses machine_thread_t plus the
 * machine_thread_* operations; it never names an architecture.
 *
 * Every flavour provides:
 *
 *   machine_thread_t                    embedded in struct thread
 *   machine_thread_init_kernel/_user    initial state of a new thread
 *   machine_thread_set_user_state       replace entry, stack and argument
 *   machine_thread_has_user_state
 *   machine_thread_user_pc/_user_sp     the initial user entry and stack
 *   machine_thread_prepare_context      first kernel context of a new thread
 *   machine_thread_switch_context       save one kernel context, restore
 *                                       another; returns in the restored one
 *   machine_thread_enter_user           drop to user mode for the first time;
 *                                       "returns" only when the user program
 *                                       has exited (see the flavour's header)
 *
 * and, through the fields of machine_thread_t, `context.sp` (the saved kernel
 * stack pointer, zeroed when the kernel stack is released).
 */

#ifndef NXU_MACH_MACHINE_THREAD_H
#define NXU_MACH_MACHINE_THREAD_H

#if defined(__aarch64__)
#include <mach/arm64/thread.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <mach/i386/thread.h>
#else
#error "machine/thread.h: unsupported target architecture"
#endif

#endif
