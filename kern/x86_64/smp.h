/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/smp.h
 *
 * Single-CPU x86-64 machine-local storage used until APIC and per-CPU GS
 * setup are added to this architecture port.
 */
#ifndef NXU_KERN_X86_64_SMP_H
#define NXU_KERN_X86_64_SMP_H

#include <stdbool.h>
#include <stdint.h>

extern void *g_x86_cpu_local;

/*
 * Routine:     machine_cpu_local
 * Purpose:
 *              Return the boot CPU's registered processor-local object.
 */
static inline void *
machine_cpu_local(void)
{
	return __atomic_load_n(&g_x86_cpu_local, __ATOMIC_ACQUIRE);
}

/*
 * Routine:     machine_cpu_local_set
 * Purpose:
 *              Publish the processor-local object for the current boot CPU.
 */
static inline void
machine_cpu_local_set(void *local)
{
	__atomic_store_n(&g_x86_cpu_local, local, __ATOMIC_RELEASE);
}

/*
 * Routine:     machine_cpu_id
 * Purpose:
 *              Return the logical CPU number from the registered local
 *              object, or zero before scheduler registration.
 */
static inline uint32_t
machine_cpu_id(void)
{
	const uint32_t *local = machine_cpu_local();

	return local != 0 ? *local : 0U;
}

/*
 * Routine:     machine_cpu_mpidr
 * Purpose:
 *              Return the boot CPU's initial APIC identifier from CPUID.
 */
static inline uint64_t
machine_cpu_mpidr(void)
{
	uint32_t eax = 1U;
	uint32_t ebx;

	__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx) : : "ecx", "edx");

	return (uint64_t)(ebx >> 24U);
}

void machine_ipi_raise(uint32_t cpu, uint32_t vector);
void machine_ipi_raise_others(uint32_t vector);

#endif
