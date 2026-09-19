/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/cpu.h
 *
 * x86 implementation of <mach/machine/cpu.h>, shared by the i386 and x86_64
 * builds. hlt is the only way to stop the core: with interrupts enabled it
 * resumes on the next interrupt (the idle loop), with them masked it stays
 * stopped (fatal-error parking).
 */

#ifndef NXU_MACH_I386_CPU_H
#define NXU_MACH_I386_CPU_H

static inline void cpu_relax(void)
{
	__asm__ volatile("pause" : : : "memory");
}

static inline void cpu_wait_for_interrupt(void)
{
	__asm__ volatile("hlt" : : : "memory");
}

static inline void cpu_wait_for_event(void)
{
	__asm__ volatile("hlt" : : : "memory");
}

#endif
