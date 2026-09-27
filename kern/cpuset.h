/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/cpuset.h
 *
 * A set of logical CPUs: thread affinity, "CPUs this address space has been
 * active on", the CPUs an IPI is sent to.
 *
 * It is an array of words sized from NXU_MAX_CPUS, not one uint64_t, so
 * raising the limit changes a constant and no caller. Logical CPU numbers
 * are dense (0 is the boot CPU) and are NOT MPIDR values: the platform layer
 * maps between them.
 */
#ifndef NXU_KERN_CPUSET_H
#define NXU_KERN_CPUSET_H

#include <stdbool.h>
#include <stdint.h>

/* The most CPUs this kernel drives. A platform with more has the extras left parked. */
#define NXU_MAX_CPUS 8U

#define NXU_CPUSET_WORDS ((NXU_MAX_CPUS + 63U) / 64U)

/* 8-byte aligned so the *_atomic helpers below are single atomics on i386 too. */
typedef struct {
	uint64_t words[NXU_CPUSET_WORDS] __attribute__((aligned(8)));
} nxu_cpuset_t;

static inline void cpuset_clear(nxu_cpuset_t *set)
{
	for (uint32_t index = 0U; index < NXU_CPUSET_WORDS; index++) set->words[index] = 0ULL;
}

/* The set of CPUs 0 .. count - 1. */
static inline void cpuset_fill(nxu_cpuset_t *set, uint32_t count)
{
	cpuset_clear(set);

	for (uint32_t cpu = 0U; cpu < count && cpu < NXU_MAX_CPUS; cpu++) {
		set->words[cpu / 64U] |= 1ULL << (cpu % 64U);
	}
}

static inline void cpuset_add(nxu_cpuset_t *set, uint32_t cpu)
{
	if (cpu < NXU_MAX_CPUS) set->words[cpu / 64U] |= 1ULL << (cpu % 64U);
}

static inline void cpuset_remove(nxu_cpuset_t *set, uint32_t cpu)
{
	if (cpu < NXU_MAX_CPUS) set->words[cpu / 64U] &= ~(1ULL << (cpu % 64U));
}

static inline bool cpuset_contains(const nxu_cpuset_t *set, uint32_t cpu)
{
	return cpu < NXU_MAX_CPUS && (set->words[cpu / 64U] & (1ULL << (cpu % 64U))) != 0ULL;
}

static inline bool cpuset_empty(const nxu_cpuset_t *set)
{
	for (uint32_t index = 0U; index < NXU_CPUSET_WORDS; index++) {
		if (set->words[index] != 0ULL) return false;
	}

	return true;
}

static inline void cpuset_intersect(nxu_cpuset_t *result, const nxu_cpuset_t *left, const nxu_cpuset_t *right)
{
	for (uint32_t index = 0U; index < NXU_CPUSET_WORDS; index++) result->words[index] = left->words[index] & right->words[index];
}

static inline uint32_t cpuset_count(const nxu_cpuset_t *set)
{
	uint32_t count = 0U;

	for (uint32_t index = 0U; index < NXU_CPUSET_WORDS; index++) count += (uint32_t)__builtin_popcountll(set->words[index]);
	return count;
}

/*
 * Shared sets (an address space's "CPUs it has run on") are updated from
 * several CPUs at once, so these change one bit atomically. Release/acquire
 * order the bit against the state it announces.
 */
static inline void cpuset_add_atomic(nxu_cpuset_t *set, uint32_t cpu)
{
	if (cpu < NXU_MAX_CPUS) __atomic_fetch_or(&set->words[cpu / 64U], 1ULL << (cpu % 64U), __ATOMIC_RELEASE);
}

static inline void cpuset_remove_atomic(nxu_cpuset_t *set, uint32_t cpu)
{
	if (cpu < NXU_MAX_CPUS) __atomic_fetch_and(&set->words[cpu / 64U], ~(1ULL << (cpu % 64U)), __ATOMIC_RELEASE);
}

static inline bool cpuset_contains_atomic(const nxu_cpuset_t *set, uint32_t cpu)
{
	return cpu < NXU_MAX_CPUS && (__atomic_load_n(&set->words[cpu / 64U], __ATOMIC_ACQUIRE) & (1ULL << (cpu % 64U))) != 0ULL;
}

#endif
