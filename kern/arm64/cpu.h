/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/arm64/cpu.h
 *
 * arm64 implementation of <kern/machine/cpu.h>.
 */

#ifndef NXU_KERN_ARM64_CPU_H
#define NXU_KERN_ARM64_CPU_H

static inline void
cpu_relax(void)
{
	__asm__ volatile("yield");
}

static inline void
cpu_wait_for_interrupt(void)
{
	__asm__ volatile("wfi");
}

/*
 * Called with IRQs masked. wfi wakes on a pending interrupt even while it is
 * masked, so nothing that arrived after the caller's last check can be slept
 * through; unmasking then takes it. Returns with IRQs unmasked.
 */
static inline void
cpu_idle_halt(void)
{
	__asm__ volatile("wfi\n\tmsr daifclr, #2" : : : "memory");
}

static inline void
cpu_wait_for_event(void)
{
	__asm__ volatile("wfe");
}

#endif
