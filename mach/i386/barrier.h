/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/barrier.h
 *
 * x86 flavour of <mach/machine/barrier.h>, shared by the i386 and x86_64
 * builds. x86 is total-store-ordered and PCI DMA is cache coherent, so a
 * store or load barrier only has to stop the compiler; the full barrier is a
 * locked read-modify-write of the stack top, which works on every CPU
 * (MFENCE needs SSE2, which a bare qemu32 model may not report).
 */

#ifndef NXU_MACH_I386_BARRIER_H
#define NXU_MACH_I386_BARRIER_H

static inline void ml_dma_wmb(void)
{
	__asm__ volatile("" : : : "memory");
}

static inline void ml_dma_rmb(void)
{
	__asm__ volatile("" : : : "memory");
}

static inline void ml_dma_mb(void)
{
#if defined(__x86_64__)
	__asm__ volatile("lock; addl $0, (%%rsp)" : : : "memory", "cc");
#else
	__asm__ volatile("lock; addl $0, (%%esp)" : : : "memory", "cc");
#endif
}

static inline void ml_cpu_relax(void)
{
	__asm__ volatile("pause" : : : "memory");
}

#endif
