#ifndef NXU_ARCH_ARM64_THREAD_H
#define NXU_ARCH_ARM64_THREAD_H

#include <mach/arm64/transition.h>

#include <stdbool.h>
#include <stdint.h>

#define ARM64_THREAD_SPSR_EL0T 0x0000000000000000ULL

/*
 * arm64_kernel_context_t
 *
 * Saved kernel context used by the scheduler when switching execution from
 * one thread to another.
 *
 * A normal AArch64 procedure call may destroy x0-x18. The scheduler therefore
 * preserves the ABI callee-saved register set x19-x30 together with SP_EL1.
 * Full asynchronous architectural state remains the responsibility of
 * arm64_exception_frame_t.
 */
typedef struct {
	uint64_t x19;
	uint64_t x20;
	uint64_t x21;
	uint64_t x22;
	uint64_t x23;
	uint64_t x24;
	uint64_t x25;
	uint64_t x26;
	uint64_t x27;
	uint64_t x28;
	uint64_t fp;
	uint64_t lr;
	uint64_t sp;
} arm64_kernel_context_t;

/*
 * arm64_user_state_t
 *
 * Initial EL0 execution state owned by one thread. Once a thread is blocked
 * inside an exception path, its live general-register state remains on that
 * thread's kernel stack in arm64_exception_frame_t.
 */
typedef struct {
	uint64_t pc;
	uint64_t sp;
	uint64_t spsr;
	uint64_t x0;
	bool valid;
} arm64_user_state_t;

/*
 * machine_thread_t
 *
 * Machine-dependent state embedded in struct thread.
 */
typedef struct machine_thread {
	arm64_kernel_context_t context;
	arm64_user_state_t user;
} machine_thread_t;

/*
 * machine_thread_init_kernel
 *
 * Initialize machine-dependent state for a kernel-only thread.
 */
void machine_thread_init_kernel(machine_thread_t *machine);

/*
 * machine_thread_init_user
 *
 * Initialize machine-dependent state for an AArch64 EL0 thread. arg becomes
 * the thread's initial x0, the pthread-create-style argument a spawned
 * thread's entry function receives.
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
 * Replace the initial EL0 PC, stack and argument-register state.
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
 * scheduler restore returns to entry while executing on stack_top.
 */
bool machine_thread_prepare_context(
	machine_thread_t *machine,
	uint64_t stack_top,
	uint64_t entry
);

/*
 * arm64_switch_context
 *
 * Save the current kernel callee-saved context into old_context and restore
 * new_context, including SP_EL1. The function returns in the restored thread.
 */
void arm64_switch_context(
	arm64_kernel_context_t *old_context,
	const arm64_kernel_context_t *new_context
);

/*
 * The machine-neutral operations behind <mach/machine/thread.h>. They are
 * thin wrappers so the scheduler and thread code do not name arm64 symbols.
 */

/*
 * machine_thread_user_pc / machine_thread_user_sp
 *
 * The initial EL0 program counter and stack pointer.
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
 * machine_thread_switch_context
 *
 * Save the current kernel context into old_machine and restore
 * new_machine's. Returns in the restored thread.
 */
static inline void machine_thread_switch_context(
	machine_thread_t *old_machine,
	machine_thread_t *new_machine
)
{
	arm64_switch_context(&old_machine->context, &new_machine->context);
}

/*
 * machine_thread_enter_user
 *
 * Enter EL0 with the thread's initial user state. Returns only when the EL0
 * program has exited through the SYS_exit redirect.
 */
static inline void machine_thread_enter_user(machine_thread_t *machine)
{
	arm64_enter_el0(
		machine->user.pc,
		machine->user.sp,
		machine->user.spsr,
		machine->user.x0
	);
}

#endif
