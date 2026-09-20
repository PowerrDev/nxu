/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/thread.h
 *
 * Machine-dependent thread state for 32-bit x86: the i386 flavour of
 * <kern/machine/thread.h>, mirroring kern/arm64/thread.h operation for
 * operation.
 *
 * Kernel context. A thread that is not running has its callee-saved
 * registers (ebp, ebx, esi, edi) and its return address on its own kernel
 * stack; context.sp is the stack pointer that points at them. Switching is
 * a push, a stack-pointer swap and a pop (context_switch.S), so a new
 * thread is started by fabricating exactly the frame a switched-out thread
 * would have.
 *
 * User state. A user thread's first entry to ring 3 is an iret through a
 * frame built from user.pc/sp/eflags/arg (transition.S). From then on the
 * thread's live user registers live in the x86_saved_state32_t on its
 * kernel stack, as on arm64.
 *
 * The kernel stack pointer the CPU loads on a ring 3 -> ring 0 trap (TSS
 * esp0) is per thread: `esp0` below, installed by every context switch.
 * For a thread that has entered user mode it is the address just below the
 * kernel context saved by machine_thread_enter_user(), so a trap frame
 * never overwrites that context.
 */

#ifndef NXU_KERN_I386_THREAD_H
#define NXU_KERN_I386_THREAD_H

#include <kern/i386/gdt.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(__i386__)
#error "kern/i386/thread.h: the x86_64 thread state is not implemented yet"
#endif

/* eflags for a new user thread: IF set, the always-one bit 1, IOPL 0. */
#define I386_THREAD_EFLAGS_USER 0x00000202U

/*
 * i386_kernel_context_t
 *
 * sp is the saved kernel stack pointer of a thread that is not running.
 * The registers themselves sit on that stack; see context_switch.S.
 */
typedef struct {
	uint32_t sp;
} i386_kernel_context_t;

/*
 * i386_user_state_t
 *
 * Initial ring 3 state of a thread. arg is the initial eax, the
 * pthread-create-style argument a spawned thread's entry function receives;
 * every other general register starts at zero.
 */
typedef struct {
	uint32_t pc;
	uint32_t sp;
	uint32_t eflags;
	uint32_t arg;
	bool valid;
} i386_user_state_t;

/*
 * machine_thread_t
 *
 * Machine-dependent state embedded in struct thread.
 *
 * esp0            stack the CPU switches to on a trap from ring 3
 * user_kernel_sp  where machine_thread_enter_user() parked the kernel
 *                 context it will resume when the user program has exited
 */
typedef struct machine_thread {
	i386_kernel_context_t context;
	i386_user_state_t user;
	uint32_t esp0;
	uint32_t user_kernel_sp;
} machine_thread_t;

/* transition.S and context_switch.S hard-code these offsets. */
_Static_assert(offsetof(machine_thread_t, context.sp) == 0U, "machine_thread context.sp offset changed");
_Static_assert(offsetof(machine_thread_t, user.pc) == 4U, "machine_thread user.pc offset changed");
_Static_assert(offsetof(machine_thread_t, user.sp) == 8U, "machine_thread user.sp offset changed");
_Static_assert(offsetof(machine_thread_t, user.eflags) == 12U, "machine_thread user.eflags offset changed");
_Static_assert(offsetof(machine_thread_t, user.arg) == 16U, "machine_thread user.arg offset changed");
_Static_assert(offsetof(machine_thread_t, user_kernel_sp) == 28U, "machine_thread user_kernel_sp offset changed");

/*
 * machine_thread_init_kernel
 *
 * Initialize machine-dependent state for a kernel-only thread.
 */
void machine_thread_init_kernel(machine_thread_t *machine);

/*
 * machine_thread_init_user
 *
 * Initialize machine-dependent state for a ring 3 thread. arg becomes the
 * thread's initial eax. entry, stack and arg must fit in 32 bits.
 */
bool machine_thread_init_user(
	machine_thread_t *machine,
	uint64_t entry,
	uint64_t stack,
	uint64_t arg
);

/*
 * machine_thread_set_user_state
 *
 * Replace the initial ring 3 eip, esp and eax.
 */
bool machine_thread_set_user_state(
	machine_thread_t *machine,
	uint64_t entry,
	uint64_t stack,
	uint64_t arg
);

/*
 * machine_thread_has_user_state
 */
bool machine_thread_has_user_state(
	const machine_thread_t *machine
);

/*
 * machine_thread_prepare_context
 *
 * Construct the first kernel context for a never-run thread. The first
 * scheduler restore returns to entry (a function taking no arguments and
 * never returning) while executing on stack_top, which must be 16-byte
 * aligned. Also records stack_top as the thread's TSS esp0.
 */
bool machine_thread_prepare_context(
	machine_thread_t *machine,
	uint64_t stack_top,
	uint64_t entry
);

/*
 * machine_thread_user_pc / machine_thread_user_sp
 *
 * The initial ring 3 eip and esp.
 */
static inline uint64_t machine_thread_user_pc(const machine_thread_t *machine)
{
	return machine->user.pc;
}

static inline uint64_t machine_thread_user_sp(const machine_thread_t *machine)
{
	return machine->user.sp;
}

/*
 * i386_switch_context
 *
 * Push ebp, ebx, esi and edi, store the resulting stack pointer through
 * old_sp, load new_sp, pop the same four registers and return on the new
 * stack. Returns in the thread that owns new_sp.
 */
void i386_switch_context(uint32_t *old_sp, uint32_t new_sp);

/*
 * i386_enter_user
 *
 * Save the kernel callee-saved registers, park the resulting stack pointer
 * in machine->user_kernel_sp, and iret to ring 3 with the thread's initial
 * user state. Returns only through i386_user_return().
 */
void i386_enter_user(machine_thread_t *machine);

/*
 * i386_user_return
 *
 * Not a C function: the address a trap frame is redirected to (with
 * user_kernel_sp in eax) to make i386_enter_user() return. See
 * i386_trap_user_terminate().
 */
void i386_user_return(void);

/*
 * machine_thread_switch_context
 *
 * Save the current kernel context into old_machine and restore
 * new_machine's, after pointing TSS esp0 at the incoming thread's kernel
 * stack. Returns in the restored thread.
 */
static inline void machine_thread_switch_context(
	machine_thread_t *old_machine,
	machine_thread_t *new_machine
)
{
	if (new_machine->esp0 != 0U) i386_gdt_set_kernel_stack(new_machine->esp0);

	i386_switch_context(&old_machine->context.sp, new_machine->context.sp);
}

/*
 * machine_thread_enter_user
 *
 * Enter ring 3 with the thread's initial user state. Returns only when the
 * user program has exited or been terminated.
 */
static inline void machine_thread_enter_user(machine_thread_t *machine)
{
	i386_enter_user(machine);
}

/*
 * i386_thread_current_machine
 *
 * The machine state of the running thread, or 0 before the scheduler is up.
 * Used by the trap layer, which has no thread pointer of its own.
 */
machine_thread_t *i386_thread_current_machine(void);

/*
 * i386_thread_user_entry_prepare
 *
 * Called by i386_enter_user() once the kernel context is parked: records
 * esp0 as the thread's trap stack and installs it in the TSS.
 */
void i386_thread_user_entry_prepare(machine_thread_t *machine, uint32_t esp0);

#endif
