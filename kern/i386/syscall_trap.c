/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/syscall_trap.c
 *
 * Strong definitions of the trap extension points that belong to the
 * thread/syscall area, the i386 counterpart of the syscall, user-fault and
 * preempt-on-return handling in kern/arm64/exception.c:
 *
 *   i386_trap_syscall         int $0x80: frame -> syscall_request_t ->
 *                             syscall_dispatch -> eax
 *   i386_trap_user_exception  a processor exception taken in ring 3
 *                             terminates the offending process
 *   i386_trap_exit            preempt on the way back to the interrupted
 *                             context
 *
 * See syscall_trap.h for the user ABI.
 */

#include <kern/i386/syscall_trap.h>

#include <kern/i386/gdt.h>
#include <kern/i386/thread.h>
#include <kern/i386/trap.h>

#include <kern/console/console.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <kern/syscall/syscall.h>

#include <stdbool.h>
#include <stdint.h>

/* eflags of a frame that resumes in the kernel: interrupts masked. */
#define I386_EFLAGS_KERNEL_RESUME 0x00000002U

static i386_user_exception_hook_t g_user_exception_hook;

void i386_trap_set_user_exception_hook(i386_user_exception_hook_t hook)
{
	g_user_exception_hook = hook;
}

/*
 * The exit path redirects the frame with these, so the kernel state reloaded
 * by the stub is well-formed even though the CPU was in ring 3 a moment ago.
 */
bool i386_trap_user_terminate(x86_saved_state_t *state)
{
	if (!x86_saved_state_is_user(state)) return false;

	machine_thread_t *machine = i386_thread_current_machine();

	if (machine == 0 || machine->user_kernel_sp == 0U) return false;

	state->eax = machine->user_kernel_sp;
	state->eip = (uint32_t)(uintptr_t)i386_user_return;
	state->cs = GDT_KERNEL_CODE_SEL;
	state->efl = I386_EFLAGS_KERNEL_RESUME;
	state->ds = GDT_KERNEL_DATA_SEL;
	state->es = GDT_KERNEL_DATA_SEL;
	/*
	 * Not GDT_KERNEL_DATA_SEL: %fs is this CPU's percpu segment throughout
	 * kernel execution (see gdt.h's file comment), and this frame resumes
	 * in the kernel (cs above is GDT_KERNEL_CODE_SEL, not a ring-3
	 * selector). i386_trap_common's epilogue pops this straight into %fs;
	 * leaving it GDT_KERNEL_DATA_SEL here left the code that runs right
	 * after (freeing the exiting thread, picking the next one to run) with
	 * a %fs that reads current_processor() as a null-page dereference
	 * instead of this CPU's processor object -- reliably, immediately
	 * after any user process's exit(), including on a single-CPU boot.
	 */
	state->fs = GDT_PERCPU_SEL;
	state->gs = GDT_KERNEL_DATA_SEL;

	return true;
}

void i386_syscall_request_from_state(const x86_saved_state_t *state, syscall_request_t *request)
{
	/* The 32-bit registers zero-extend into the 64-bit request fields. */
	*request = (syscall_request_t) {
		.number = state->eax,
		.arguments = {
			state->ebx,
			state->ecx,
			state->edx,
			state->esi,
			state->edi,
			state->ebp
		}
	};
}

void i386_trap_syscall(x86_saved_state_t *state)
{
	syscall_request_t request;

	i386_syscall_request_from_state(state, &request);

	syscall_result_t result = syscall_dispatch(&request);

	switch (result.action) {
	case SYSCALL_ACTION_RETURN:
		state->eax = (uint32_t)result.value;
		return;

	case SYSCALL_ACTION_EXIT:
		kputs("syscall: exit(");
		kputu64(result.value);
		kputln(")");

		if (!i386_trap_user_terminate(state)) {
			kputln("i386_trap_syscall: exit from a frame that cannot be unwound");
			state->eax = I386_SYSCALL_UNIMPLEMENTED;
		}

		return;

	default:
		state->eax = I386_SYSCALL_UNIMPLEMENTED;
		return;
	}
}

static const char *user_exception_name(uint32_t vector)
{
	switch (vector) {
	case T_DIVIDE_ERROR: return "#DE Divide Error";
	case T_DEBUG: return "#DB Debug";
	case T_BREAKPOINT: return "#BP Breakpoint";
	case T_OVERFLOW: return "#OF Overflow";
	case T_BOUND_RANGE: return "#BR Bound Range Exceeded";
	case T_INVALID_OPCODE: return "#UD Invalid Opcode";
	case T_DEVICE_NOT_AVAILABLE: return "#NM Device Not Available";
	case T_INVALID_TSS: return "#TS Invalid TSS";
	case T_SEGMENT_NOT_PRESENT: return "#NP Segment Not Present";
	case T_STACK_FAULT: return "#SS Stack-Segment Fault";
	case T_GENERAL_PROTECTION: return "#GP General Protection Fault";
	case T_PAGE_FAULT: return "#PF Page Fault";
	case T_X87_FAULT: return "#MF x87 Floating-Point Error";
	case T_ALIGNMENT_CHECK: return "#AC Alignment Check";
	case T_SIMD_FAULT: return "#XM SIMD Floating-Point Exception";
	default: return "exception";
	}
}

static void user_exception_report(const x86_saved_state_t *state, proc_t proc)
{
	char name[PROC_NAME_MAX];

	name[0] = '\0';
	if (proc != 0) (void)proc_get_name(proc, name, sizeof(name));

	kprintf(
		"i386_trap_user_exception: pid %u (%s) terminated by %s at 0x%x:0x%x\n",
		(uint32_t)proc_selfpid(),
		name,
		user_exception_name(state->trapno),
		state->cs & 0xFFFFU,
		state->eip
	);

	if (state->trapno == T_PAGE_FAULT) {
		kprintf(
			"  cause: %s of 0x%x by user code: %s\n",
			(state->err & PF_FETCH) != 0U ? "instruction fetch" : ((state->err & PF_WRITE) != 0U ? "write" : "read"),
			state->cr2,
			(state->err & PF_PRESENT) != 0U ? "protection violation" : "page not present"
		);
	} else if (state->trapno == T_GENERAL_PROTECTION || state->trapno == T_SEGMENT_NOT_PRESENT || state->trapno == T_STACK_FAULT) {
		kprintf("  error code 0x%x\n", state->err);
	}

	kprintf(
		"  eax=%x ebx=%x ecx=%x edx=%x\n  esi=%x edi=%x ebp=%x esp=%x\n  eip=%x efl=%x cs=%x ss=%x\n",
		state->eax,
		state->ebx,
		state->ecx,
		state->edx,
		state->esi,
		state->edi,
		state->ebp,
		state->uesp,
		state->eip,
		state->efl,
		state->cs & 0xFFFFU,
		state->ss & 0xFFFFU
	);
}

bool i386_trap_user_exception(x86_saved_state_t *state)
{
	if (g_user_exception_hook != 0 && g_user_exception_hook(state)) return true;

	proc_t proc = current_proc();

	/* Nothing to terminate: the kernel's own task has no ring 3 code. */
	if (proc == 0 || proc == proc_kernel()) return false;

	user_exception_report(state, proc);

	/*
	 * Terminate the whole process as SYS_exit would, then unwind this
	 * thread's ring 3 excursion. The status marks it as a kernel kill.
	 */
	if (!proc_exit_current(I386_USER_FAULT_STATUS_BASE | state->trapno)) return false;

	return i386_trap_user_terminate(state);
}

void i386_trap_exit(x86_saved_state_t *state)
{
	/*
	 * Interrupts are masked here (every vector is an interrupt gate). As on
	 * arm64, a switch is only safe when the interrupted context holds no
	 * kernel state a preemption could leave half done: user code, or the
	 * idle thread. Kernel threads are cooperative.
	 */
	if (!sched_preemption_pending()) return;

	thread_t thread = current_thread();
	bool scheduler_safe =
		x86_saved_state_is_user(state) ||
		(thread != 0 && thread_is_idle(thread));

	if (!scheduler_safe) return;

	if (!sched_preempt()) sched_clear_preemption();
}
