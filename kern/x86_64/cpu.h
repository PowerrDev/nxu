/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/cpu.h
 *
 * x86-64 CPU hints used by machine-independent spin and idle loops.
 * Keep these separate from the i386 implementation so long-mode-only
 * primitives can be added here without changing the 32-bit port.
 */

#ifndef NXU_KERN_X86_64_CPU_H
#define NXU_KERN_X86_64_CPU_H

static inline void
cpu_relax(void)
{
	__asm__ volatile("pause" : : : "memory");
}

static inline void
cpu_wait_for_interrupt(void)
{
	__asm__ volatile("hlt" : : : "memory");
}

/* As kern/i386/cpu.h's: called with interrupts masked; sti's one-instruction shadow covers the hlt. */
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

#endif
