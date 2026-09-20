#ifndef NXU_ARCH_ARM64_SYSTEM_H
#define NXU_ARCH_ARM64_SYSTEM_H

#include <stdint.h>

/*
 * Read CurrentEL. The architectural exception-level number lives in bits 3:2,
 * so callers that need the decoded EL should use arm_read_current_el().
 */
static inline uint64_t arm_read_current_el_raw(void)
{
	uint64_t value;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(value));
	return value;
}

static inline uint32_t arm_read_current_el(void)
{
	return (uint32_t)((arm_read_current_el_raw() >> 2U) & 0x3U);
}

/* Read the CPU implementer/part/revision identification register. */
static inline uint64_t arm64_read_midr_el1(void)
{
	uint64_t value;
	__asm__ volatile("mrs %0, MIDR_EL1" : "=r"(value));
	return value;
}

/* Read the affinity information used to identify this processing element. */
static inline uint64_t arm64_read_mpidr_el1(void)
{
	uint64_t value;
	__asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(value));
	return value;
}

/* Read the current EL1 system-control state. */
static inline uint64_t arm64_read_sctlr_el1(void)
{
	uint64_t value;
	__asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(value));
	return value;
}

/* Read interrupt/debug mask state from DAIF. */
static inline uint64_t arm64_read_daif(void)
{
	uint64_t value;
	__asm__ volatile("mrs %0, DAIF" : "=r"(value));
	return value;
}

/* Install the EL1 exception-vector base and synchronize instruction execution. */
static inline void arm64_write_vbar_el1(uint64_t address)
{
	__asm__ volatile(
		"msr VBAR_EL1, %0\n"
		"isb"
		:
		: "r"(address)
		: "memory"
	);
}

static inline uint64_t arm64_read_esr_el1(void)
{
	uint64_t value;
	__asm__ volatile("mrs %0, ESR_EL1" : "=r"(value));
	return value;
}

static inline uint64_t arm64_read_elr_el1(void)
{
	uint64_t value;
	__asm__ volatile("mrs %0, ELR_EL1" : "=r"(value));
	return value;
}

static inline uint64_t arm64_read_spsr_el1(void)
{
	uint64_t value;
	__asm__ volatile("mrs %0, SPSR_EL1" : "=r"(value));
	return value;
}

static inline uint64_t arm64_read_far_el1(void)
{
	uint64_t value;
	__asm__ volatile("mrs %0, FAR_EL1" : "=r"(value));
	return value;
}

/*
 * Save the complete DAIF value while masking IRQ delivery. The saved value can
 * later be restored exactly with arm64_irq_restore().
 */
static inline uint64_t arm64_irq_save(void)
{
	uint64_t daif;
	__asm__ volatile(
		"mrs %0, DAIF\n"
		"msr DAIFSet, #2\n"
		"isb"
		: "=r"(daif)
		:
		: "memory"
	);
	return daif;
}

static inline void arm64_irq_restore(uint64_t daif)
{
	__asm__ volatile(
		"msr DAIF, %0\n"
		"isb"
		:
		: "r"(daif)
		: "memory"
	);
}

static inline void arm64_irq_enable(void)
{
	__asm__ volatile(
		"msr DAIFClr, #2\n"
		"isb"
		:
		:
		: "memory"
	);
}

static inline void arm64_irq_disable(void)
{
	__asm__ volatile(
		"msr DAIFSet, #2\n"
		"isb"
		:
		:
		: "memory"
	);
}

/*
 * The machine-neutral operations behind <kern/machine/system.h>. Both use
 * PSCI over the hypervisor-call conduit (SYSTEM_RESET, SYSTEM_OFF). They
 * return only if the firmware refused the request.
 */
static inline void machine_system_reset(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000009ULL;
	__asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
}

static inline void machine_system_power_off(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ULL;
	__asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
}

#endif
