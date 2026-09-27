/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/machine_routines.h
 *
 * x86 implementation of <kern/machine/machine_routines.h>, shared by the
 * i386 and x86_64 builds. Interrupt state is EFLAGS/RFLAGS; only the IF bit
 * is acted on when restoring, so the other flags are never clobbered by an
 * interrupt-state restore.
 */

#ifndef NXU_KERN_I386_MACHINE_ROUTINES_H
#define NXU_KERN_I386_MACHINE_ROUTINES_H

#include <stdbool.h>
#include <stdint.h>

#define I386_EFLAGS_IF (1ULL << 9U)

static inline uint64_t i386_read_flags(void)
{
	uintptr_t flags;

#if defined(__x86_64__)
	__asm__ volatile("pushfq\n\tpopq %0" : "=r"(flags) : : "memory");
#else
	__asm__ volatile("pushfl\n\tpopl %0" : "=r"(flags) : : "memory");
#endif

	return (uint64_t)flags;
}

static inline uint64_t ml_irq_save(void)
{
	uint64_t flags = i386_read_flags();

	__asm__ volatile("cli" : : : "memory");
	return flags;
}

static inline void ml_irq_restore(uint64_t state)
{
	if ((state & I386_EFLAGS_IF) != 0ULL) {
		__asm__ volatile("sti" : : : "memory");
	} else {
		__asm__ volatile("cli" : : : "memory");
	}
}

static inline void ml_irq_enable(void)
{
	__asm__ volatile("sti" : : : "memory");
}

static inline void ml_irq_disable(void)
{
	__asm__ volatile("cli" : : : "memory");
}

/* Whether IRQs are enabled on this CPU right now (EFLAGS.IF). */
static inline bool ml_irq_enabled(void)
{
	return (i386_read_flags() & I386_EFLAGS_IF) != 0ULL;
}

#endif
