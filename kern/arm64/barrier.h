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

#include <stdint.h>

static inline void
ml_dma_wmb(void)
{
	__asm__ volatile("dmb oshst" : : : "memory");
}

static inline void
ml_dma_rmb(void)
{
	__asm__ volatile("dmb oshld" : : : "memory");
}

static inline void
ml_dma_mb(void)
{
	__asm__ volatile("dmb osh" : : : "memory");
}

static inline uint32_t
ml_dma_cache_line_size(void)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, CTR_EL0" : "=r"(ctr));
	return 4U << (uint32_t)((ctr >> 16U) & 0xFU);
}

static inline void
ml_dma_cache_clean(uint64_t start, uint64_t size)
{
	if (size == 0ULL || size > UINT64_MAX - start) return;
	uint64_t line_size = ml_dma_cache_line_size();
	uint64_t end = start + size;
	for (uint64_t address = start & ~(line_size - 1ULL); address < end; address += line_size)
		__asm__ volatile("dc cvac, %0" : : "r"(address) : "memory");
	__asm__ volatile("dsb osh" : : : "memory");
}

static inline void
ml_dma_cache_clean_invalidate(uint64_t start, uint64_t size)
{
	if (size == 0ULL || size > UINT64_MAX - start) return;
	uint64_t line_size = ml_dma_cache_line_size();
	uint64_t end = start + size;
	for (uint64_t address = start & ~(line_size - 1ULL); address < end; address += line_size)
		__asm__ volatile("dc civac, %0" : : "r"(address) : "memory");
	__asm__ volatile("dsb osh" : : : "memory");
}

static inline void
ml_dma_cache_invalidate(uint64_t start, uint64_t size)
{
	if (size == 0ULL || size > UINT64_MAX - start) return;
	uint64_t line_size = ml_dma_cache_line_size();
	uint64_t end = start + size;
	for (uint64_t address = start & ~(line_size - 1ULL); address < end; address += line_size)
		__asm__ volatile("dc ivac, %0" : : "r"(address) : "memory");
	__asm__ volatile("dsb osh" : : : "memory");
}

static inline void
ml_cpu_relax(void)
{
	__asm__ volatile("yield");
}

#endif
