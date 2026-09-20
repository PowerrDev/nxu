#include <kern/console/console.h>
#include <kern/arm64/exception.h>
#include <kern/arm64/gic.h>
#include <kern/arm64/system.h>
#include <kern/arm64/timer.h>
#include <kern/arm64/transition.h>
#include <kern/sched_prism/sched.h>
#include <kern/irq/irq.h>
#include <kern/syscall/syscall.h>
#include <kern/process/proc.h>
#include <kern/process/signal.h>
#include <kern/process/thread.h>
#include <kern/arm64/user.h>
#include <kern/logging/version.h>
#include <kern/machine/vm_param.h>
#include <platform/uart.h>
#include <vm/vm_fault.h>

#include <stddef.h>
#include <stdint.h>

#define ARM64_EC_UNKNOWN 0x00U
#define ARM64_EC_ILLEGAL_STATE 0x0EU
#define ARM64_EC_SVC64 0x15U
#define ARM64_EC_IABT_LOWER 0x20U
#define ARM64_EC_PC_ALIGNMENT 0x22U
#define ARM64_EC_DABT_LOWER 0x24U
#define ARM64_EC_DABT_CURRENT 0x25U
#define ARM64_EC_SP_ALIGNMENT 0x26U
#define ARM64_EC_FP64 0x2CU
#define ARM64_EC_BRK64 0x3CU

/* Data/instruction fault status codes (ISS[5:0]) that are not translation,
 * access-flag or permission faults. */
#define ARM64_FSC_SYNC_EXTERNAL_ABORT 0x10U
#define ARM64_FSC_ALIGNMENT 0x21U

/* Translation (0x04-0x07), access-flag (0x08-0x0B) and permission
 * (0x0C-0x0F) faults are the ones the page-fault resolver can fix. */
#define ARM64_FSC_RESOLVABLE_FIRST 0x04U
#define ARM64_FSC_RESOLVABLE_LAST 0x0FU

/* Data-abort ISS: WnR, set when the faulting access was a write. */
#define ARM64_ISS_WNR (1U << 6U)

#define ARM64_SVC_SYSCALL_IMMEDIATE 0U
#define ARM64_SPSR_EL1H_MASKED 0x3C5ULL

extern const char exception_vectors[];

_Static_assert(
	sizeof(arm64_exception_frame_t) == 304U,
	"exception frame size must match exception_vectors.S"
);

_Static_assert(
	offsetof(arm64_exception_frame_t, sp) == 248U,
	"exception frame SP offset mismatch"
);

_Static_assert(
	offsetof(arm64_exception_frame_t, elr) == 256U,
	"exception frame ELR offset mismatch"
);

_Static_assert(
	offsetof(arm64_exception_frame_t, spsr) == 264U,
	"exception frame SPSR offset mismatch"
);

_Static_assert(
	offsetof(arm64_exception_frame_t, far) == 272U,
	"exception frame FAR offset mismatch"
);

_Static_assert(
	offsetof(arm64_exception_frame_t, esr) == 280U,
	"exception frame ESR offset mismatch"
);

_Static_assert(
	offsetof(arm64_exception_frame_t, vector_id) == 288U,
	"exception frame vector offset mismatch"
);

_Static_assert(
	offsetof(arm64_exception_frame_t, sp_el0) == 296U,
	"exception frame SP_EL0 offset mismatch"
);

static bool g_panic_active;

static uint8_t exception_class(uint64_t esr)
{
	return (uint8_t)((esr >> 26U) & 0x3FU);
}

static uint8_t exception_instruction_length(uint64_t esr)
{
	return (uint8_t)((esr >> 25U) & 0x1U);
}

static uint32_t exception_iss(uint64_t esr)
{
	return (uint32_t)(esr & 0x01FFFFFFU);
}

/*
 * These print helpers deliberately do not return const char pointers.
 *
 * NXU is linked at its permanent higher-half VMA, but early boot executes
 * the same bytes through their physical alias before TTBR1 is enabled. Static
 * pointer tables already contain higher-half addresses, so direct PC-relative
 * printing keeps the panic path usable during both bootstrap and runtime.
 */
static __attribute__((noinline, optnone))
void exception_print_name(uint8_t ec)
{
	if (ec == 0x00U) {
		kputs("Undefined Instruction");
		return;
	}

	if (ec == 0x15U) {
		kputs("Supervisor Call");
		return;
	}

	if (ec == 0x20U || ec == 0x21U) {
		kputs("Instruction Abort");
		return;
	}

	if (ec == 0x24U || ec == 0x25U) {
		kputs("Data Abort");
		return;
	}

	if (ec == 0x22U) {
		kputs("PC Alignment Fault");
		return;
	}

	if (ec == 0x26U) {
		kputs("Stack Alignment Fault");
		return;
	}

	if (ec == 0x3CU) {
		kputs("Breakpoint");
		return;
	}

	kputs("Unhandled ARM64 Exception");
}

static __attribute__((noinline, optnone))
void exception_print_origin(arm64_exception_origin_t origin)
{
	if (origin == ARM64_EXCEPTION_ORIGIN_CURRENT_EL_SP0) {
		kputs("current EL using SP_EL0");
		return;
	}

	if (origin == ARM64_EXCEPTION_ORIGIN_CURRENT_EL_SPX) {
		kputs("current EL using SP_EL1");
		return;
	}

	if (origin == ARM64_EXCEPTION_ORIGIN_LOWER_EL_AARCH64) {
		kputs("lower EL using AArch64");
		return;
	}

	if (origin == ARM64_EXCEPTION_ORIGIN_LOWER_EL_AARCH32) {
		kputs("lower EL using AArch32");
		return;
	}

	kputs("invalid vector source");
}

static __attribute__((noinline, optnone))
void exception_print_instruction_length(uint8_t il)
{
	if (il != 0U) {
		kputs("32-bit instruction");
		return;
	}

	kputs("16-bit or unknown");
}

static __attribute__((noinline, optnone))
void exception_print_trap_origin(uint64_t vector_id)
{
	if (exception_vector_from_aarch64_el0(vector_id)) {
		kputs("User trap type ");
		return;
	}

	kputs("Kernel trap type ");
}

/*
 * These are NXU panic categories, not the architectural ESR exception
 * class numbers.
 */
static uint8_t panic_trap_type(uint8_t ec)
{
	switch (ec) {
	case 0x00:
		return 1U;

	case 0x20:
	case 0x21:
		return 2U;

	case 0x24:
	case 0x25:
		return 3U;

	case 0x15:
		return 4U;

	default:
		return 0U;
	}
}

static __attribute__((noreturn))
void panic_halt(void)
{
	__asm__ volatile(
		"msr daifset, #0xf"
		:
		:
		: "memory"
	);

	for (;;) {
		__asm__ volatile("wfe");
	}
}

static void kput_small_decimal(uint32_t value)
{
	if (value >= 10U) {
		kputc((char)('0' + value / 10U));
	}

	kputc((char)('0' + value % 10U));
}

static void panic_print_x_register(uint32_t index, uint64_t value)
{
	/* The extra spaces align x0-x9 with x10-x28. */
	if (index < 10U) {
		kputs("    x");
	} else {
		kputs("   x");
	}

	kput_small_decimal(index);
	kputs(": ");
	kputhex64(value);
}

static uint64_t exception_interrupted_stack_pointer(
	const arm64_exception_frame_t *frame
)
{
	return exception_vector_from_aarch64_el0(frame->vector_id)
		? frame->sp_el0
		: frame->sp;
}

static void panic_print_thread_state(
	const arm64_exception_frame_t *frame
)
{
	kputln("ARM Thread State (64-bit):");

	for (uint32_t index = 0U; index < 27U; index += 3U) {
		panic_print_x_register(index, frame->x[index]);
		kputs("   ");

		panic_print_x_register(index + 1U, frame->x[index + 1U]);
		kputs("   ");

		panic_print_x_register(index + 2U, frame->x[index + 2U]);
		kputc('\n');
	}

	panic_print_x_register(27U, frame->x[27]);
	kputs("   ");

	panic_print_x_register(28U, frame->x[28]);
	kputc('\n');

	kputs("    fp: ");
	kputhex64(frame->x[29]);

	kputs("   lr: ");
	kputhex64(frame->x[30]);
	kputc('\n');

	kputs("    sp: ");
	kputhex64(exception_interrupted_stack_pointer(frame));

	kputs("   pc: ");
	kputhex64(frame->elr);

	kputs("  cpsr: ");
	kputhex32((uint32_t)frame->spsr);
	kputc('\n');

	kputs("   far: ");
	kputhex64(frame->far);

	kputs("  esr: ");
	kputhex32((uint32_t)frame->esr);
	kputc('\n');
}

void exception_init(void)
{
	arm64_write_vbar_el1((uint64_t)exception_vectors);
}

arm64_exception_kind_t exception_vector_kind(uint64_t vector_id)
{
	if (vector_id >= 16ULL) {
		return ARM64_EXCEPTION_KIND_INVALID;
	}

	return (arm64_exception_kind_t)(vector_id & 0x3ULL);
}

arm64_exception_origin_t exception_vector_origin(uint64_t vector_id)
{
	if (vector_id >= 16ULL) {
		return ARM64_EXCEPTION_ORIGIN_INVALID;
	}

	return (arm64_exception_origin_t)(vector_id >> 2U);
}

bool exception_vector_from_aarch64_el0(uint64_t vector_id)
{
	return exception_vector_origin(vector_id) == ARM64_EXCEPTION_ORIGIN_LOWER_EL_AARCH64;
}

bool exception_validate_vector_classification(void)
{
	return
		exception_vector_kind(4ULL) ==
			ARM64_EXCEPTION_KIND_SYNCHRONOUS &&
		exception_vector_origin(4ULL) ==
			ARM64_EXCEPTION_ORIGIN_CURRENT_EL_SPX &&
		exception_vector_kind(5ULL) ==
			ARM64_EXCEPTION_KIND_IRQ &&
		exception_vector_origin(5ULL) ==
			ARM64_EXCEPTION_ORIGIN_CURRENT_EL_SPX &&
		exception_vector_kind(8ULL) ==
			ARM64_EXCEPTION_KIND_SYNCHRONOUS &&
		exception_vector_from_aarch64_el0(8ULL) &&
		exception_vector_kind(9ULL) ==
			ARM64_EXCEPTION_KIND_IRQ &&
		exception_vector_from_aarch64_el0(9ULL) &&
		exception_vector_kind(16ULL) ==
			ARM64_EXCEPTION_KIND_INVALID &&
		exception_vector_origin(16ULL) == ARM64_EXCEPTION_ORIGIN_INVALID;
}

static void irq_handle(void)
{
	uint32_t intid = gic_acknowledge_interrupt();

	/* Values 1020-1023 are special or spurious INTIDs. */
	if (intid >= 1020U) {
		return;
	}

	if (intid == 30U) {
		timer_handle_interrupt();
		sched_tick();
	} else if (!irq_dispatch(intid)) {
		kputs("irq: unhandled INTID ");
		kputu64(intid);
		kputc('\n');
	}

	gic_end_interrupt(intid);
}

/*
 * exception_unwind_to_kernel
 *
 * The process is gone: instead of returning to EL0, resume in
 * arm64_return_from_el0, which restores the kernel registers arm64_enter_el0
 * parked and returns from it, so the dying thread falls out of its EL0
 * excursion and exits.
 */
static void exception_unwind_to_kernel(arm64_exception_frame_t *frame)
{
	frame->elr = arm64_el0_return_address();
	frame->spsr = ARM64_SPSR_EL1H_MASKED;
}

static bool exception_handle_user_svc(arm64_exception_frame_t *frame)
{
	if (
		!exception_vector_from_aarch64_el0(frame->vector_id) ||
		exception_class(frame->esr) != ARM64_EC_SVC64
	) {
		return false;
	}

	uint16_t immediate = (uint16_t)(exception_iss(frame->esr) & 0xFFFFU);

	if (immediate != ARM64_SVC_SYSCALL_IMMEDIATE) {
		return false;
	}

	syscall_request_t request = {
		.number = frame->x[8],
		.arguments = {
			frame->x[0],
			frame->x[1],
			frame->x[2],
			frame->x[3],
			frame->x[4],
			frame->x[5]
		}
	};

	syscall_result_t result = syscall_dispatch(&request);

	switch (result.action) {
	case SYSCALL_ACTION_RETURN:
		frame->x[0] = result.value;
		return true;

	case SYSCALL_ACTION_EXIT:
		kputs("syscall: exit(");
		kputu64(result.value);
		kputln(")");

		exception_unwind_to_kernel(frame);

		return true;

	case SYSCALL_ACTION_KEEP_FRAME:
		/* exec / sigreturn already rewrote the user registers. */
		return true;

	default:
		return false;
	}
}

/*
 * exception_user_fault_signal
 *
 * Map a synchronous exception taken from EL0 to the signal POSIX would raise
 * for it. Anything unrecognised is treated as an illegal instruction: EL0
 * has no business causing it and SIGILL is the least misleading answer.
 */
static uint32_t exception_user_fault_signal(const arm64_exception_frame_t *frame)
{
	uint32_t fault_status = exception_iss(frame->esr) & 0x3FU;

	switch (exception_class(frame->esr)) {
	case ARM64_EC_IABT_LOWER:
	case ARM64_EC_DABT_LOWER:
		if (
			fault_status == ARM64_FSC_ALIGNMENT ||
			fault_status == ARM64_FSC_SYNC_EXTERNAL_ABORT
		) {
			return NXU_SIGBUS;
		}

		return NXU_SIGSEGV;

	case ARM64_EC_PC_ALIGNMENT:
	case ARM64_EC_SP_ALIGNMENT:
		return NXU_SIGBUS;

	case ARM64_EC_FP64:
		return NXU_SIGFPE;

	case ARM64_EC_BRK64:
		return NXU_SIGTRAP;

	case ARM64_EC_UNKNOWN:
	case ARM64_EC_ILLEGAL_STATE:
	default:
		return NXU_SIGILL;
	}
}

static const char *exception_signal_name(uint32_t signal)
{
	switch (signal) {
	case NXU_SIGSEGV: return "SIGSEGV";
	case NXU_SIGBUS: return "SIGBUS";
	case NXU_SIGFPE: return "SIGFPE";
	case NXU_SIGTRAP: return "SIGTRAP";
	default: return "SIGILL";
	}
}

/*
 * exception_resolve_page_fault
 *
 * A data or instruction abort whose cause the page-fault resolver may be able
 * to remove: an untouched demand-zero page, or a write to a page fork left
 * shared copy-on-write. On success the faulting instruction has nothing left
 * to trip over, so returning from the exception simply retries it.
 */
static bool exception_resolve_page_fault(const arm64_exception_frame_t *frame, bool instruction)
{
	uint32_t fault_status = exception_iss(frame->esr) & 0x3FU;

	if (
		fault_status < ARM64_FSC_RESOLVABLE_FIRST ||
		fault_status > ARM64_FSC_RESOLVABLE_LAST
	) {
		return false;
	}

	/* The space the MMU actually walked: for an EL0 fault that is the
	 * current process's, and it is also right for kernel code running a
	 * self-test against a scratch space with no process behind it. */
	vm_address_space_t *space = (vm_address_space_t *)vm_address_space_current();

	if (space == 0 || !vm_address_space_is_active(space)) {
		return false;
	}

	vm_fault_access_t access = instruction
		? VM_FAULT_EXECUTE
		: ((exception_iss(frame->esr) & ARM64_ISS_WNR) != 0U ? VM_FAULT_WRITE : VM_FAULT_READ);

	return vm_fault_user(space, frame->far, access);
}

/*
 * exception_handle_user_fault
 *
 * A synchronous exception from EL0 that is not a system call: a bad
 * pointer, a wild jump, an undefined instruction. This is the process's
 * fault, never the kernel's, so it must not reach the panic path. Report it,
 * terminate the whole process exactly the way exit() does, and unwind this
 * thread's EL0 excursion through the same return trampoline.
 *
 * Returns false only when there is no user process to blame (the fault came
 * from the kernel task, or termination was refused); the caller then panics.
 */
static bool exception_handle_user_fault(arm64_exception_frame_t *frame)
{
	if (!exception_vector_from_aarch64_el0(frame->vector_id)) {
		return false;
	}

	proc_t proc = current_proc();

	if (proc == 0 || proc == proc_kernel()) {
		return false;
	}

	uint32_t exception_class_value = exception_class(frame->esr);

	if (
		(exception_class_value == ARM64_EC_DABT_LOWER || exception_class_value == ARM64_EC_IABT_LOWER) &&
		exception_resolve_page_fault(frame, exception_class_value == ARM64_EC_IABT_LOWER)
	) {
		return true;
	}

	uint32_t signal = exception_user_fault_signal(frame);

	/*
	 * A program that installed a handler for the signal gets to run it --
	 * unless it has the signal blocked, in which case (or if the handler
	 * frame cannot be built) there is no safe way to continue and the
	 * process is killed, as POSIX leaves undefined.
	 */
	thread_t faulting_thread = current_thread();

	if (faulting_thread != 0 && (faulting_thread->sig_blocked & (1U << signal)) == 0U) {
		nxu_sigaction_t action = proc->p_sigact[signal];

		if (action.handler > NXU_SIG_IGN) {
			uint32_t previous_mask = faulting_thread->sig_blocked;

			if (machine_user_signal_push(action.handler, action.restorer, signal, previous_mask)) {
				faulting_thread->sig_blocked = signal_blocked_mask_for_handler(previous_mask, signal, &action);
				return true;
			}
		}
	}

	char name[PROC_NAME_MAX];

	name[0] = '\0';
	(void)proc_get_name(proc, name, sizeof(name));

	kprintf(
		"exception_handle_user_fault: pid %u (%s) killed by %s, pc 0x%llx, far 0x%llx, esr 0x%llx\n",
		(uint32_t)proc_selfpid(),
		name,
		exception_signal_name(signal),
		(unsigned long long)frame->elr,
		(unsigned long long)frame->far,
		(unsigned long long)frame->esr
	);

	if (!proc_exit_current(NXU_EXIT_KILLED_SIGNAL(signal))) {
		return false;
	}

	exception_unwind_to_kernel(frame);

	return true;
}

/*
 * exception_handle_kernel_user_access
 *
 * Kernel code dereferenced a user address (a self-test or a syscall reading
 * user memory directly rather than through vm_copy_*) and hit a page the
 * process has reserved but never touched. Resolve it exactly as an EL0 fault
 * would be; anything outside the current process's own user range is a
 * kernel bug and stays a panic.
 */
static bool exception_handle_kernel_user_access(arm64_exception_frame_t *frame)
{
	if (
		exception_vector_origin(frame->vector_id) != ARM64_EXCEPTION_ORIGIN_CURRENT_EL_SPX ||
		exception_class(frame->esr) != ARM64_EC_DABT_CURRENT ||
		frame->far >= VM_MAX_USER_ADDRESS
	) {
		return false;
	}

	return exception_resolve_page_fault(frame, false);
}

/*
 * exception_deliver_pending_signals
 *
 * On the way back to EL0: take the next signal this thread does not block
 * and act on it. Ignored ones are dropped, the default action terminates the
 * process, and a caught one has its handler frame pushed so the thread
 * "returns" into the handler. One handler at a time -- the rest wait for the
 * handler's sigreturn, which comes back through here.
 */
static void exception_deliver_pending_signals(arm64_exception_frame_t *frame)
{
	proc_t proc = current_proc();
	thread_t thread = current_thread();

	if (
		proc == 0 ||
		proc == proc_kernel() ||
		thread == 0 ||
		!thread_is_active(thread) ||
		(proc->p_flag & PROC_FLAG_EXITING) != 0U
	) {
		return;
	}

	for (;;) {
		uint32_t signal = signal_dequeue(proc, thread);

		if (signal == 0U) return;

		nxu_sigaction_t action = proc->p_sigact[signal];
		uint32_t terminate_with = 0U;

		if (signal == NXU_SIGKILL) {
			terminate_with = NXU_SIGKILL;
		} else if (action.handler == NXU_SIG_IGN) {
			continue;
		} else if (action.handler == NXU_SIG_DFL) {
			if (signal_default_action(signal) == SIGNAL_DEFAULT_IGNORE) continue;
			terminate_with = signal;
		} else {
			uint32_t previous_mask = thread->sig_blocked;

			if (machine_user_signal_push(action.handler, action.restorer, signal, previous_mask)) {
				thread->sig_blocked = signal_blocked_mask_for_handler(previous_mask, signal, &action);
				return;
			}

			/* No room for the handler frame (stack overflow, bad stack). */
			terminate_with = NXU_SIGSEGV;
		}

		if (proc_exit_current(NXU_EXIT_KILLED_SIGNAL(terminate_with))) {
			exception_unwind_to_kernel(frame);
		}

		return;
	}
}

static void exception_dispatch(arm64_exception_frame_t *frame)
{
	arm64_exception_kind_t kind = exception_vector_kind(frame->vector_id);

	arm64_exception_origin_t origin = exception_vector_origin(frame->vector_id);

	if (kind == ARM64_EXCEPTION_KIND_IRQ) {
		irq_handle();

		thread_t thread = current_thread();
		bool scheduler_safe =
			exception_vector_from_aarch64_el0(frame->vector_id) ||
			(thread != 0 && thread_is_idle(thread));

		if (scheduler_safe && sched_preemption_pending()) {
			if (!sched_preempt()) {
				sched_clear_preemption();
			}
		}

		return;
	}

	if (
		kind == ARM64_EXCEPTION_KIND_SYNCHRONOUS &&
		exception_handle_user_svc(frame)
	) {
		return;
	}

	if (
		kind == ARM64_EXCEPTION_KIND_SYNCHRONOUS &&
		exception_handle_kernel_user_access(frame)
	) {
		return;
	}

	if (
		kind == ARM64_EXCEPTION_KIND_SYNCHRONOUS &&
		exception_handle_user_fault(frame)
	) {
		return;
	}

	/*
	 * Never recursively panic. If the panic path itself faults, immediately
	 * stop instead of consuming the EL1 stack with nested exception frames.
	 */
	if (g_panic_active) {
		panic_halt();
	}

	g_panic_active = true;

	uint8_t ec = exception_class(frame->esr);
	uint8_t trap_type = panic_trap_type(ec);
	uint8_t il = exception_instruction_length(frame->esr);
	uint32_t iss = exception_iss(frame->esr);

	kputc('\n');

	/*
	 * For now, "caller" displays the faulting PC. Later, symbol lookup and
	 * stack unwinding can replace it with a resolved panic caller.
	 */
	kputs("panic(cpu 0 caller ");
	kputhex64(frame->elr);
	kputs("): \"");

	exception_print_trap_origin(frame->vector_id);

	kputc((char)('0' + trap_type));
	kputs(" -- ");
	exception_print_name(ec);
	kputln("\"");

	kputln("Debugger message: panic");

	kputs("OS version: ");
	kputs(NXU_KERNEL_NAME);
	kputc(' ');
	kputln(NXU_VERSION);

	kputs("Kernel version: ");
	kputln(NXU_KERNEL_VERSION);

	kputc('\n');

	panic_print_thread_state(frame);

	kputc('\n');
	kputln("Exception Syndrome:");

	kputs("    Source: ");
	exception_print_origin(origin);
	kputc('\n');

	kputs("    EC:  ");
	kputhex_byte(ec);
	kputs(" (");
	exception_print_name(ec);
	kputln(")");

	kputs("    IL:  ");
	exception_print_instruction_length(il);
	kputc('\n');

	kputs("    ISS: ");
	kputhex32(iss);
	kputc('\n');

	kputc('\n');
	kputln("Kernel Extensions in backtrace: none");
	kprintf("System uptime: %llu microseconds\n", (unsigned long long)timer_get_microseconds());
	kprintf("Timer interrupts delivered: %llu\n", (unsigned long long)timer_get_interrupt_count());
	kputln("NXU panic policy: interrupts masked, processor halted after diagnostic emission");
	kputln("Kernel halted.");

	panic_halt();
}

/*
 * exception_handle
 *
 * Entry from the vector table. For a trap from EL0 it records the frame on
 * the thread for as long as the thread is inside the kernel -- fork, exec
 * and signal delivery read and rewrite the user's registers through it --
 * and, once the trap is handled, delivers any signal now due before the
 * thread returns to user mode.
 */
void exception_handle(arm64_exception_frame_t *frame)
{
	thread_t thread = exception_vector_from_aarch64_el0(frame->vector_id)
		? current_thread()
		: 0;

	if (thread != 0) {
		thread->machine.user_frame = frame;
	}

	exception_dispatch(frame);

	if (thread != 0) {
		exception_deliver_pending_signals(frame);
		thread->machine.user_frame = 0;
	}
}
