/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/cpu.h
 *
 * x86 implementation of <kern/machine/cpu.h>, shared by the i386 and x86_64
 * builds. hlt is the only way to stop the core: with interrupts enabled it
 * resumes on the next interrupt (the idle loop), with them masked it stays
 * stopped (fatal-error parking).
 */

#ifndef NXU_KERN_I386_CPU_H
#define NXU_KERN_I386_CPU_H

#include <stdint.h>

#if defined(__i386__)
/*
 * TLB shootdown poll (kern/i386/smp.c). Every spin loop goes through
 * cpu_relax(), and syscalls, traps and the kernel VM locks all run with
 * interrupts masked, so a CPU spinning there could never take the shootdown
 * IPI of a CPU that is waiting on it. Servicing a pending request here breaks
 * that cycle. Weak, so code linked without smp.c still builds.
 */
extern uint32_t g_tlb_shootdown_targets __attribute__((weak));
void smp_tlb_shootdown_handle(void) __attribute__((weak));
#endif

static inline void
cpu_relax(void)
{
	__asm__ volatile("pause" : : : "memory");
#if defined(__i386__)
	if (&g_tlb_shootdown_targets != 0 && __atomic_load_n(&g_tlb_shootdown_targets, __ATOMIC_RELAXED) != 0U &&
		smp_tlb_shootdown_handle != 0)
		smp_tlb_shootdown_handle();
#endif
}

static inline void
cpu_wait_for_interrupt(void)
{
	__asm__ volatile("hlt" : : : "memory");
}

/*
 * Called with interrupts masked. sti takes effect only after the next
 * instruction, so nothing can be taken between it and the hlt: an interrupt
 * already pending wakes the hlt at once instead of being handled just before
 * it and leaving the core asleep. Returns with interrupts enabled, after one
 * has been taken.
 */
static inline void
cpu_idle_halt(void)
{
	__asm__ volatile("sti\n\thlt" : : : "memory");
}

static inline void
cpu_wait_for_event(void)
{
	__asm__ volatile("hlt" : : : "memory");
}

/* CPUID leaf `leaf`, subleaf 0: eax, ebx, ecx, edx in that order. */
static inline void
cpu_cpuid(uint32_t leaf, uint32_t registers[4])
{
	uint32_t eax = leaf;
	uint32_t ecx = 0U;
	uint32_t ebx;
	uint32_t edx;

	__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
	registers[0] = eax;
	registers[1] = ebx;
	registers[2] = ecx;
	registers[3] = edx;
}

/* Read and write a model-specific register. Faults (#GP) on one the CPU does not have: check CPUID first. */
static inline uint64_t
cpu_rdmsr(uint32_t msr)
{
	uint32_t low;
	uint32_t high;

	__asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((uint64_t)high << 32U) | low;
}

static inline void
cpu_wrmsr(uint32_t msr, uint64_t value)
{
	__asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32U)) : "memory");
}

#endif
