#include <kern/console/console.h>
#include <mach/arm64/exception.h>
#include <mach/arm64/gic.h>
#include <mach/arm64/system.h>
#include <mach/arm64/timer.h>
#include <mach/arm64/transition.h>
#include <kern/sched_prism/sched.h>
#include <kern/irq/irq.h>
#include <kern/syscall/syscall.h>
#include <kern/process/thread.h>
#include <kern/logging/version.h>
#include <platform/uart.h>

#include <stddef.h>
#include <stdint.h>


#define ARM64_EC_SVC64 0x15U

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

		frame->elr = arm64_el0_return_address();
		frame->spsr = ARM64_SPSR_EL1H_MASKED;

		return true;

	default:
		return false;
	}
}

void exception_handle(arm64_exception_frame_t *frame)
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
