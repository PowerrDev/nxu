/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/thread.c
 *
 * See thread.h. Implements the machine-thread operations that
 * kern/process/thread.c and kern/sched_prism/sched.c call, on 32-bit x86.
 */

#include <mach/i386/thread.h>

#include <kern/process/thread.h>

#include <stdbool.h>
#include <stdint.h>

/* transition.S hard-codes the user selectors. */
_Static_assert(GDT_USER_CODE_SEL == 0x1B, "transition.S user code selector out of date");
_Static_assert(GDT_USER_DATA_SEL == 0x23, "transition.S user data selector out of date");

/* Words in the initial kernel frame: edi, esi, ebx, ebp, return address, dummy caller. */
#define I386_INITIAL_FRAME_WORDS 6U

static void machine_thread_zero(machine_thread_t *machine)
{
	*machine = (machine_thread_t) {
		.context = { .sp = 0U },
		.user = {
			.pc = 0U,
			.sp = 0U,
			.eflags = I386_THREAD_EFLAGS_USER,
			.arg = 0U,
			.valid = false
		},
		.esp0 = 0U,
		.user_kernel_sp = 0U
	};
}

/* The 32-bit port cannot address anything above 4 GiB. */
static bool machine_thread_fits(uint64_t value)
{
	return (value >> 32U) == 0ULL;
}

void machine_thread_init_kernel(machine_thread_t *machine)
{
	if (machine == 0) return;
	machine_thread_zero(machine);
}

bool machine_thread_init_user(
	machine_thread_t *machine,
	uint64_t entry,
	uint64_t stack,
	uint64_t arg
)
{
	if (machine == 0 || entry == 0ULL || stack == 0ULL) return false;

	machine_thread_zero(machine);
	return machine_thread_set_user_state(machine, entry, stack, arg);
}

bool machine_thread_set_user_state(
	machine_thread_t *machine,
	uint64_t entry,
	uint64_t stack,
	uint64_t arg
)
{
	if (machine == 0 || entry == 0ULL || stack == 0ULL) return false;
	if (!machine_thread_fits(entry) || !machine_thread_fits(stack) || !machine_thread_fits(arg)) return false;

	machine->user.pc = (uint32_t)entry;
	machine->user.sp = (uint32_t)stack;
	machine->user.eflags = I386_THREAD_EFLAGS_USER;
	machine->user.arg = (uint32_t)arg;
	machine->user.valid = true;

	return true;
}

bool machine_thread_has_user_state(const machine_thread_t *machine)
{
	return machine != 0 && machine->user.valid;
}

bool machine_thread_prepare_context(
	machine_thread_t *machine,
	uint64_t stack_top,
	uint64_t entry
)
{
	if (
		machine == 0 ||
		stack_top == 0ULL ||
		entry == 0ULL ||
		!machine_thread_fits(stack_top) ||
		!machine_thread_fits(entry) ||
		(stack_top & 0xFULL) != 0ULL
	) {
		return false;
	}

	/*
	 * Build what i386_switch_context() would have left behind, so the first
	 * restore "returns" into entry:
	 *
	 *   stack_top -  4   0      dummy return address for entry
	 *   stack_top -  8   entry  popped by the ret in i386_switch_context
	 *   stack_top - 12   0      ebp
	 *   stack_top - 16   0      ebx
	 *   stack_top - 20   0      esi
	 *   stack_top - 24   0      edi   <- context.sp
	 *
	 * After the ret, esp is stack_top - 4: entry starts as if it had been
	 * called, with esp + 4 16-byte aligned as the i386 System V ABI wants.
	 */
	uint32_t *frame = (uint32_t *)(uintptr_t)((uint32_t)stack_top - I386_INITIAL_FRAME_WORDS * 4U);

	frame[0] = 0U;
	frame[1] = 0U;
	frame[2] = 0U;
	frame[3] = 0U;
	frame[4] = (uint32_t)entry;
	frame[5] = 0U;

	machine->context.sp = (uint32_t)(uintptr_t)frame;
	machine->esp0 = (uint32_t)stack_top;
	machine->user_kernel_sp = 0U;

	return true;
}

machine_thread_t *i386_thread_current_machine(void)
{
	thread_t thread = current_thread();

	return thread == 0 ? 0 : &thread->machine;
}

void i386_thread_user_entry_prepare(machine_thread_t *machine, uint32_t esp0)
{
	machine->esp0 = esp0;
	i386_gdt_set_kernel_stack(esp0);
}
