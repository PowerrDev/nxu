#ifndef NXU_ARCH_ARM64_TRANSITION_H
#define NXU_ARCH_ARM64_TRANSITION_H

#include <stdint.h>

/**
 * arm64_enter_higher_half - Enter the TTBR1 kernel mapping.
 * @offset: Higher-half direct-map offset.
 * @vector_base: Higher-half VBAR_EL1 address.
 * @entry: Higher-half continuation address.
 *
 * Masks exceptions, converts SP_EL1 to its higher alias, installs the
 * higher vector base and branches to the supplied continuation.
 *
 * This function never returns.
 */
__attribute__((noreturn))
void arm64_enter_higher_half(
	uint64_t offset,
	uint64_t vector_base,
	uint64_t entry
);

static inline uint64_t arm64_read_stack_pointer(void)
{
	uint64_t value;

	__asm__ volatile("mov %0, sp" : "=r"(value));

	return value;
}

static inline uint64_t arm64_read_program_counter(void)
{
	uint64_t value;

	__asm__ volatile("adr %0, ." : "=r"(value));

	return value;
}

static inline uint64_t arm64_read_vector_base(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, VBAR_EL1" : "=r"(value));

	return value;
}

void arm64_enter_el0(uint64_t entry, uint64_t stack, uint64_t spsr, uint64_t arg);

uint64_t arm64_el0_return_address(void);

#endif
