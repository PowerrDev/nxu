/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/arm64/barrier.h
 *
 * AArch64 flavour of <kern/machine/barrier.h>: outer-shareable DMB, which is
 * what orders CPU accesses against a DMA master.
 */

#ifndef NXU_KERN_ARM64_BARRIER_H
#define NXU_KERN_ARM64_BARRIER_H

static inline void ml_dma_wmb(void)
{
	__asm__ volatile("dmb oshst" : : : "memory");
}

static inline void ml_dma_rmb(void)
{
	__asm__ volatile("dmb oshld" : : : "memory");
}

static inline void ml_dma_mb(void)
{
	__asm__ volatile("dmb osh" : : : "memory");
}

static inline void ml_cpu_relax(void)
{
	__asm__ volatile("yield");
}

#endif
