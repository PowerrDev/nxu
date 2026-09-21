/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/smp.h
 *
 * i386 implementation of <kern/machine/smp.h>. The i386 port is uniprocessor:
 * there is no per-CPU register to keep the object in, so it reports "none set"
 * and the machine-independent code falls back to the one boot processor.
 */
#ifndef NXU_KERN_I386_SMP_H
#define NXU_KERN_I386_SMP_H

#include <stdint.h>

static inline void *machine_cpu_local(void)
{
	return 0;
}

static inline void machine_cpu_local_set(void *local)
{
	(void)local;
}

static inline uint32_t machine_cpu_id(void)
{
	return 0U;
}

static inline uint64_t machine_cpu_mpidr(void)
{
	return 0ULL;
}

#endif
