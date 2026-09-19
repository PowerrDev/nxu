/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/cache.h
 *
 * Cache maintenance for x86. The instruction and data caches are coherent
 * (a store to memory that is about to be executed is seen by the fetch unit
 * without any explicit maintenance, and self-modifying code needs only a
 * serializing instruction, which the return to ring 3 already is), and the
 * caches are enabled from reset, so every routine here is a no-op or reports
 * the fixed x86 answer. The names match <mach/arm64/cache.h> so shared code
 * (kern/loader) is written once.
 */

#ifndef NXU_MACH_I386_CACHE_H
#define NXU_MACH_I386_CACHE_H

#include <stdbool.h>
#include <stdint.h>

/* x86 caches are on out of reset (CR0.CD/NW are clear); nothing to bring up. */
static inline bool cache_init(void)
{
	return true;
}

static inline bool cache_instruction_enabled(void)
{
	return true;
}

static inline bool cache_data_enabled(void)
{
	return true;
}

/* 64 bytes on every part NXU targets; only a stride hint for maintenance loops. */
static inline uint32_t cache_instruction_line_size(void)
{
	return 64U;
}

static inline uint32_t cache_data_line_size(void)
{
	return 64U;
}

static inline void cache_dump(void)
{
}

/* Instruction fetch is coherent with data stores: nothing to publish. */
static inline void cache_sync_instruction_range(uint64_t address, uint64_t size)
{
	(void)address;
	(void)size;
}

#endif
