/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/arm64/user.h
 *
 * arm64 implementation of the user-context operations declared in
 * <mach/machine/user.h>, working through the exception frame the calling
 * thread trapped with (machine_thread_t::user_frame).
 *
 * The signal frame the kernel pushes on the user stack:
 *
 *     sp -> magic     identifies a frame this kernel built
 *           x0..x30   the interrupted registers
 *           sp, pc    the interrupted stack pointer and program counter
 *           spsr      the interrupted PSTATE (only the condition flags are
 *                     ever restored)
 *           mask      the thread's blocked-signal mask before delivery
 *
 * The handler runs with x0 = signal, x30 = the sigaction's restorer, and
 * sp pointing at this frame, so the restorer's sigreturn finds it there.
 */

#ifndef NXU_MACH_ARM64_USER_H
#define NXU_MACH_ARM64_USER_H

#include <mach/arm64/thread.h>

#include <stdbool.h>
#include <stdint.h>

#define MACHINE_USER_CONTEXT 1

bool machine_user_fork_state(machine_thread_t *child);
bool machine_user_exec_state(uint64_t entry, uint64_t stack, uint64_t argc, uint64_t argv);
bool machine_user_signal_push(uint64_t handler, uint64_t restorer, uint32_t signal, uint32_t saved_mask);
bool machine_user_signal_pop(uint32_t *saved_mask);

#endif
