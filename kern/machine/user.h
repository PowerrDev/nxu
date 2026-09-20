/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/machine/user.h
 *
 * Architecture dispatch for operations on a thread's user-mode register
 * state, in the manner of XNU's <machine/...> headers. fork, exec and signal
 * delivery need to read and rewrite the registers a user program trapped
 * with; machine-independent code does that only through these calls.
 *
 * Every function acts on the calling thread while it is inside the kernel
 * for a trap from user mode (a system call, a fault or a preemption) and
 * returns false when it is not, or when the port cannot do it.
 *
 *   MACHINE_USER_CONTEXT
 *       1 when the port implements all of the below, 0 when it does not
 *       (fork, exec and signals then fail with "not supported").
 *
 *   machine_user_fork_state(child)
 *       Make child resume the way the caller's current trap will: same
 *       registers, same stack, same place -- except that its fork() result
 *       register reads 0.
 *
 *   machine_user_exec_state(entry, stack, argc, argv)
 *       Reset the calling thread's user context to the start of a fresh
 *       program: the given entry point and stack, every other register zero,
 *       argc and argv in the first two argument registers.
 *
 *   machine_user_signal_push(handler, restorer, signal, saved_mask)
 *       Build a signal frame on the user stack holding the interrupted
 *       context and saved_mask, then redirect the thread to run handler
 *       with signal as its argument and restorer as its return address.
 *
 *   machine_user_signal_pop(saved_mask)
 *       Undo machine_user_signal_push (the sigreturn system call): restore
 *       the interrupted context from the frame at the user stack pointer and
 *       report the mask that was saved with it. Refuses a frame that does
 *       not look like one the kernel built.
 */

#ifndef NXU_KERN_MACHINE_USER_H
#define NXU_KERN_MACHINE_USER_H

#if defined(__aarch64__)
#include <kern/arm64/user.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <kern/i386/user.h>
#else
#error "machine/user.h: unsupported target architecture"
#endif

#endif
