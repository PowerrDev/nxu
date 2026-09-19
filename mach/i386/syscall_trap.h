/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/syscall_trap.h
 *
 * The system-call and user-fault side of the trap layer: the strong
 * definitions of the extension points declared in trap.h that belong to the
 * thread/syscall area (i386_trap_syscall, i386_trap_user_exception and
 * i386_trap_exit).
 *
 * User ABI (fixed; the userland area codes against it):
 *
 *   entry      int $0x80
 *   number     eax  (zero-extended to the 64-bit syscall_request_t number)
 *   arguments  ebx, ecx, edx, esi, edi, ebp   (arguments[0..5], in order,
 *              each zero-extended to 64 bits)
 *   result     eax, the low 32 bits of syscall_result_t.value. Errors keep
 *              the arm64 convention, a small negative number
 *              (-SYSCALL_ERROR_*, so -1 .. -12 = 0xFFFFFFFF .. 0xFFFFFFF4).
 *              A 64-bit result (uptime in microseconds, seek offsets) is
 *              truncated to 32 bits.
 *   preserved  every register except eax; the kernel does not touch the
 *              others, so a wrapper needs only "eax" and "memory" clobbers.
 *
 * A new user thread starts at eip = entry with esp = stack exactly as
 * given (no return address is pushed), eax = its argument, every other
 * general register zero, eflags = IF (0x202), user data segments loaded.
 */

#ifndef NXU_MACH_I386_SYSCALL_TRAP_H
#define NXU_MACH_I386_SYSCALL_TRAP_H

#include <mach/i386/trap.h>

#include <kern/syscall/syscall.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Exit status recorded for a process the kernel terminated because it took
 * a processor exception in ring 3: this base or'd with the vector number
 * (#GP is 0x8000000D). Ordinary exit codes never set the top bit.
 */
#define I386_USER_FAULT_STATUS_BASE 0x80000000U

/*
 * Test-only extension point. If a hook is installed it sees every
 * exception taken in ring 3 first; returning true means it consumed the
 * exception and the frame is resumed. It lets a self-test raise a software
 * "timer tick" (an `int` to an otherwise unused exception vector) and watch
 * the preempt-on-return path run for a real user thread. Pass 0 to remove.
 */
typedef bool (*i386_user_exception_hook_t)(x86_saved_state_t *state);

void i386_trap_set_user_exception_hook(i386_user_exception_hook_t hook);

/*
 * i386_syscall_request_from_state
 *
 * Build the machine-independent request from a saved frame: number from
 * eax, arguments from ebx, ecx, edx, esi, edi, ebp, all zero-extended.
 */
void i386_syscall_request_from_state(const x86_saved_state_t *state, syscall_request_t *request);

/*
 * i386_trap_user_terminate
 *
 * Redirect a ring 3 frame so the iret that resumes it lands in
 * i386_user_return and makes machine_thread_enter_user() return in the
 * thread's first-run trampoline, as the arm64 SYS_exit path does. Returns
 * false if the frame is not a user frame or its thread never entered ring 3
 * through machine_thread_enter_user().
 */
bool i386_trap_user_terminate(x86_saved_state_t *state);

#endif
