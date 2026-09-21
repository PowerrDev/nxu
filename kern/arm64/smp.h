/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/arm64/smp.h
 *
 * arm64 implementation of <kern/machine/smp.h>, and the secondary-CPU
 * bring-up interface (kern/arm64/smp.c).
 *
 * The per-CPU pointer lives in TPIDR_EL1: it is banked per CPU, EL1-only, and
 * the kernel never gives it another use, so reading the current CPU's object
 * is one `mrs`. The object's first word is the logical CPU id.
 */
#ifndef NXU_KERN_ARM64_SMP_H
#define NXU_KERN_ARM64_SMP_H

#include <stdbool.h>
#include <stdint.h>

static inline void *machine_cpu_local(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, TPIDR_EL1" : "=r"(value));
	return (void *)value;
}

static inline void machine_cpu_local_set(void *local)
{
	/*
	 * No barrier needed: the register is only ever read by the CPU that
	 * wrote it, and the ISB below orders it against what follows on this
	 * CPU (instruction fetch and system register access are in program
	 * order relative to an ISB).
	 */
	__asm__ volatile("msr TPIDR_EL1, %0\n\tisb" : : "r"((uint64_t)local) : "memory");
}

static inline uint32_t machine_cpu_id(void)
{
	const uint32_t *local = (const uint32_t *)machine_cpu_local();

	return local != 0 ? *local : 0U;
}

/* This CPU's MPIDR affinity fields (Aff3:Aff2:Aff1:Aff0), as PSCI and the GIC name a CPU. */
static inline uint64_t machine_cpu_mpidr(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(value));
	return value & 0xFF00FFFFFFULL;
}

/*
 * machine_ipi_raise / machine_ipi_raise_others
 *
 * Deliver IPI vector `vector` (an ipi_type_t, see kern/ipi.h) to logical CPU
 * `cpu`, or to every CPU but this one. On arm64 a vector is the number of a
 * GICv3 SGI, which each CPU enables in its own redistributor.
 */
void machine_ipi_raise(uint32_t cpu, uint32_t vector);
void machine_ipi_raise_others(uint32_t vector);

/*
 * smp_handle_ipi
 *
 * The IRQ handler's entry for an acknowledged SGI (INTID 0-15), on the CPU it
 * was sent to. Interrupt context.
 */
void smp_handle_ipi(uint32_t vector);

/*
 * smp_enable_local_ipis
 *
 * Enable the IPI SGIs in the calling CPU's redistributor. Every CPU calls it
 * once, after its GIC interface is up.
 */
void smp_enable_local_ipis(void);

/*
 * smp_stop_other_cpus
 *
 * Halt every other CPU (a panic is about to print). Does not wait for them
 * beyond a short bounded pause, and is safe to call more than once.
 */
void smp_stop_other_cpus(void);

/* ---- bring-up (kern/arm64/smp.c) ---------------------------------------- */

/*
 * smp_boot_secondaries
 *
 * Start every other CPU the platform lists with PSCI CPU_ON, wait for each to
 * report online, and leave it parked in a WFI loop. Called once, on the boot
 * CPU, after memory management and the GIC are up and before interrupts are
 * enabled. Returns the number of CPUs online, boot CPU included.
 */
uint32_t smp_boot_secondaries(void);

/* CPUs the platform lists (capped at NXU_MAX_CPUS), and CPUs that have reported online. */
uint32_t smp_cpu_count(void);
uint32_t smp_online_count(void);

/*
 * The MPIDR affinity of logical CPU `cpu`, or 0 if there is no such CPU. The
 * map is fixed at discovery: logical id N is the Nth CPU node in the Device
 * Tree. (0 is also the MPIDR of a real CPU, so range-check with
 * smp_cpu_count() first.)
 */
uint64_t smp_cpu_mpidr(uint32_t cpu);

#endif
