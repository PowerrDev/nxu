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
 *   i386_trap_exit            take pending signals and preempt on the way
 *                             back to the interrupted context
 *
 * See syscall_trap.h for the user ABI.
 */

#include <kern/i386/syscall_trap.h>

#include <kern/i386/gdt.h>
#include <kern/i386/thread.h>
#include <kern/i386/trap.h>

#include <kern/console/console.h>
#include <kern/i386/user.h>
#include <kern/process/proc.h>
#include <kern/process/signal.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <kern/syscall/syscall.h>

#include <stdbool.h>
#include <stdint.h>

/* eflags of a frame that resumes in the kernel: interrupts masked. */
#define I386_EFLAGS_KERNEL_RESUME 0x00000002U

static i386_user_exception_hook_t g_user_exception_hook;

void
i386_trap_set_user_exception_hook(i386_user_exception_hook_t hook)
{
	g_user_exception_hook = hook;
}

/*
 * The exit path redirects the frame with these, so the kernel state reloaded
 * by the stub is well-formed even though the CPU was in ring 3 a moment ago.
 */
bool
i386_trap_user_terminate(x86_saved_state_t *state)
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

void
i386_syscall_request_from_state(const x86_saved_state_t *state, syscall_request_t *request)
{
	/* The 32-bit registers zero-extend into the 64-bit request fields. */
	*request = (syscall_request_t){
		.number = state->eax, .arguments = {state->ebx, state->ecx, state->edx, state->esi, state->edi, state->ebp}};
}

void
i386_trap_syscall(x86_saved_state_t *state)
{
	syscall_request_t request;

	i386_syscall_request_from_state(state, &request);

	/* sigreturn rewrites the frame it was called from; this is how it finds it. */
	machine_thread_t *machine = i386_thread_current_machine();

	if (machine != 0) machine->user_frame = state;

	syscall_result_t result = syscall_dispatch(&request);

	if (machine != 0) machine->user_frame = 0;

	switch (result.action) {
	case SYSCALL_ACTION_RETURN:
		state->eax = (uint32_t)result.value;
		return;

	case SYSCALL_ACTION_KEEP_FRAME:
		/* The call replaced the whole frame (sigreturn): eax is part of what it restored. */
		return;

	case SYSCALL_ACTION_RESTART:
		/* eax and eip still describe the call as it was made; see i386_finish_restart. */
		return;

	case SYSCALL_ACTION_EXIT:
		kprintf("exit(");
		kputu64(result.value);
		kputln(")");

		if (!i386_trap_user_terminate(state)) {
			kprintf("exit from a frame that cannot be unwound\n");
			state->eax = I386_SYSCALL_UNIMPLEMENTED;
		}

		return;

	default:
		state->eax = I386_SYSCALL_UNIMPLEMENTED;
		return;
	}

}

static const char *
user_exception_name(uint32_t vector)
{
	switch (vector) {
	case T_DIVIDE_ERROR:
		return "#DE Divide Error";
	case T_DEBUG:
		return "#DB Debug";
	case T_BREAKPOINT:
		return "#BP Breakpoint";
	case T_OVERFLOW:
		return "#OF Overflow";
	case T_BOUND_RANGE:
		return "#BR Bound Range Exceeded";
	case T_INVALID_OPCODE:
		return "#UD Invalid Opcode";
	case T_DEVICE_NOT_AVAILABLE:
		return "#NM Device Not Available";
	case T_INVALID_TSS:
		return "#TS Invalid TSS";
	case T_SEGMENT_NOT_PRESENT:
		return "#NP Segment Not Present";
	case T_STACK_FAULT:
		return "#SS Stack-Segment Fault";
	case T_GENERAL_PROTECTION:
		return "#GP General Protection Fault";
	case T_PAGE_FAULT:
		return "#PF Page Fault";
	case T_X87_FAULT:
		return "#MF x87 Floating-Point Error";
	case T_ALIGNMENT_CHECK:
		return "#AC Alignment Check";
	case T_SIMD_FAULT:
		return "#XM SIMD Floating-Point Exception";
	default:
		return "exception";
	}

}

static void
user_exception_report(const x86_saved_state_t *state, proc_t proc)
{
	char name[PROC_NAME_MAX];

	name[0] = '\0';
	if (proc != 0) (void)proc_get_name(proc, name, sizeof(name));

	kprintf("pid %u (%s) terminated by %s at 0x%x:0x%x\n", (uint32_t)proc_selfpid(), name,
			user_exception_name(state->trapno), state->cs & 0xFFFFU, state->eip);

	if (state->trapno == T_PAGE_FAULT) {
		IOLog("  cause: %s of 0x%x by user code: %s\n",
				(state->err & PF_FETCH) != 0U ? "instruction fetch"
											  : ((state->err & PF_WRITE) != 0U ? "write" : "read"),
				state->cr2, (state->err & PF_PRESENT) != 0U ? "protection violation" : "page not present");
	} else if (state->trapno == T_GENERAL_PROTECTION || state->trapno == T_SEGMENT_NOT_PRESENT ||
			   state->trapno == T_STACK_FAULT) {
		IOLog("  error code 0x%x\n", state->err);
	}

	IOLog("  eax=%x ebx=%x ecx=%x edx=%x\n  esi=%x edi=%x ebp=%x esp=%x\n  eip=%x efl=%x cs=%x ss=%x\n", state->eax,
			state->ebx, state->ecx, state->edx, state->esi, state->edi, state->ebp, state->uesp, state->eip, state->efl,
			state->cs & 0xFFFFU, state->ss & 0xFFFFU);
}

bool
i386_trap_user_exception(x86_saved_state_t *state)
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
	if (!proc_exit_current(I386_USER_FAULT_STATUS_BASE | state->trapno)) {
		/*
		 * A process that is already on its way out (killed while this thread
		 * was in ring 3) has nothing left to terminate, and its fault is no
		 * reason to stop the machine: unwind the thread as for any exit.
		 * Anything else that cannot be terminated stays a panic.
		 */
		if ((proc->p_flag & PROC_FLAG_EXITING) == 0U && proc->p_stat != PROC_STATE_ZOMBIE) return false;
	}

	return i386_trap_user_terminate(state);
}

/* The length of `int $0x80`, which a restarted system call goes back over. */
#define I386_SYSCALL_INSTRUCTION_LENGTH 2U

/*
 * i386_finish_restart
 *
 * A system call a signal interrupted before it did anything: make it again by
 * pointing eip back at its `int $0x80` (eax and the argument registers still
 * hold it as it was made), or give it the interrupted result. Done before a
 * handler frame is built, so the context the frame saves is the one the
 * handler's sigreturn comes back to.
 */
static void
i386_finish_restart(x86_saved_state_t *state, thread_t thread, const nxu_sigaction_t *handler)
{
	switch (syscall_restart_resolve(thread, handler)) {
	case SYSCALL_RESTART_AGAIN:
		state->eip -= I386_SYSCALL_INSTRUCTION_LENGTH;
		break;
	case SYSCALL_RESTART_INTERRUPTED:
		state->eax = (uint32_t)(0ULL - (uint64_t)NXU_SYS_E_INTERRUPTED);
		break;
	default:
		break;
	}
}

/*
 * i386_deliver_pending_signals
 *
 * The frame is about to resume in ring 3: act on the thread's signals first.
 * signal_take parks the thread while its process is stopped and does
 * everything that needs no user context; what is left is building a handler
 * frame, or ending the process. One handler at a time: the rest wait for the
 * handler's sigreturn, which comes back through here.
 */
static void
i386_deliver_pending_signals(x86_saved_state_t *state)
{
	proc_t proc = current_proc();
	thread_t thread = current_thread();
	machine_thread_t *machine = i386_thread_current_machine();

	if (proc == 0 || proc == proc_kernel() || thread == 0 || machine == 0 || !thread_is_active(thread) ||
		(proc->p_flag & PROC_FLAG_EXITING) != 0U) {
		return;
	}

	uint32_t signal;
	nxu_sigaction_t action;
	uint32_t terminate_with;

	switch (signal_take(proc, thread, &signal, &action)) {
	case SIGNAL_TAKE_NONE:
		i386_finish_restart(state, thread, 0);
		return;
	case SIGNAL_TAKE_HANDLER: {
		uint32_t previous_mask = thread->sig_blocked;

		i386_finish_restart(state, thread, &action);
		machine->user_frame = state;

		bool pushed = machine_user_signal_push(action.handler, action.restorer, signal, previous_mask);

		machine->user_frame = 0;

		if (pushed) {
			thread->sig_blocked = signal_blocked_mask_for_handler(previous_mask, signal, &action);
			return;
		}

		/* No room for the handler frame (stack overflow, bad stack). */
		terminate_with = NXU_SIGSEGV;
		break;
	}
	default:
		terminate_with = signal;
		break;
	}

	if (proc_exit_current(NXU_EXIT_KILLED_SIGNAL(terminate_with))) (void)i386_trap_user_terminate(state);
}

void
i386_trap_exit(x86_saved_state_t *state)
{
	/*
	 * Every way back to ring 3 passes here, system call or interrupt, so a
	 * signal posted to a thread that is running user code is acted on at the
	 * next interrupt it takes. A frame that was just redirected into the
	 * kernel (the process exited) is no longer a user frame and is skipped.
	 */
	if (x86_saved_state_is_user(state)) i386_deliver_pending_signals(state);

	/*
	 * Interrupts are masked here (every vector is an interrupt gate). As on
	 * arm64, a switch is only safe when the interrupted context holds no
	 * kernel state a preemption could leave half done: user code, or the
	 * idle thread. Kernel threads are cooperative.
	 */
	if (!sched_preemption_pending()) return;

	thread_t thread = current_thread();
	bool scheduler_safe = x86_saved_state_is_user(state) || (thread != 0 && thread_is_idle(thread));

	if (!scheduler_safe) return;

	if (!sched_preempt()) sched_clear_preemption();
}
