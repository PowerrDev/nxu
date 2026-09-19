#include <arch/arm64/thread.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert(
	offsetof(arm64_kernel_context_t, x19) == 0U,
	"arm64 context x19 offset mismatch"
);

_Static_assert(
	offsetof(arm64_kernel_context_t, x21) == 16U,
	"arm64 context x21 offset mismatch"
);

_Static_assert(
	offsetof(arm64_kernel_context_t, x23) == 32U,
	"arm64 context x23 offset mismatch"
);

_Static_assert(
	offsetof(arm64_kernel_context_t, x25) == 48U,
	"arm64 context x25 offset mismatch"
);

_Static_assert(
	offsetof(arm64_kernel_context_t, x27) == 64U,
	"arm64 context x27 offset mismatch"
);

_Static_assert(
	offsetof(arm64_kernel_context_t, fp) == 80U,
	"arm64 context fp offset mismatch"
);

_Static_assert(
	offsetof(arm64_kernel_context_t, sp) == 96U,
	"arm64 context sp offset mismatch"
);

_Static_assert(
	sizeof(arm64_kernel_context_t) == 104U,
	"arm64 kernel context size mismatch"
);

/*
 * machine_thread_zero
 */
static void machine_thread_zero(machine_thread_t *machine)
{
	*machine = (machine_thread_t) {
		.context = { 0 },
		.user = {
			.pc = 0ULL,
			.sp = 0ULL,
			.spsr = ARM64_THREAD_SPSR_EL0T,
			.valid = false
		}
	};
}

/*
 * machine_thread_init_kernel
 */
void machine_thread_init_kernel(machine_thread_t *machine)
{
	if (machine == 0) return;
	machine_thread_zero(machine);
}

/*
 * machine_thread_init_user
 */
bool machine_thread_init_user(
	machine_thread_t *machine,
	uint64_t entry,
	uint64_t stack
)
{
	if (machine == 0 || entry == 0ULL || stack == 0ULL) return false;

	machine_thread_zero(machine);
	return machine_thread_set_user_state(machine, entry, stack);
}

/*
 * machine_thread_set_user_state
 */
bool machine_thread_set_user_state(
	machine_thread_t *machine,
	uint64_t entry,
	uint64_t stack
)
{
	if (machine == 0 || entry == 0ULL || stack == 0ULL) return false;

	machine->user.pc = entry;
	machine->user.sp = stack;
	machine->user.spsr = ARM64_THREAD_SPSR_EL0T;
	machine->user.valid = true;

	return true;
}

/*
 * machine_thread_has_user_state
 */
bool machine_thread_has_user_state(const machine_thread_t *machine)
{
	return machine != 0 && machine->user.valid;
}

/*
 * machine_thread_prepare_context
 */
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
		(stack_top & 0xFULL) != 0ULL
	) {
		return false;
	}

	machine->context = (arm64_kernel_context_t) { 0 };
	machine->context.lr = entry;
	machine->context.sp = stack_top;

	return true;
}
