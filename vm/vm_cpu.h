/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/vm_cpu.h
 *
 * The VM core's view of the executing CPU. The neutral address-space core
 * needs only four things from the scheduler's per-CPU object: which CPU is
 * running (or none, early in boot), the user address space loaded on it, a way
 * to record a new one, and the CPU's logical id for the active_cpus set.
 *
 * Abstracting those behind this seam keeps vm/address_space.c independent of
 * the scheduler's concrete `struct processor` layout -- which matters because
 * a port may run the shared VM core before (or without) the sched_prism
 * scheduler. On the scheduler-bearing ports (arm64, i386) these are inline
 * accessors straight onto struct processor, so the generated code is identical
 * to touching the fields directly. On a port with no scheduler yet (the
 * nascent single-CPU x86_64) current_cpu() is always null, so the VM core runs
 * entirely on its g_boot_active_space path and the per-CPU accessors are never
 * reached; they are inline no-ops there.
 */
#ifndef NXU_VM_VM_CPU_H
#define NXU_VM_VM_CPU_H

#include <stdint.h>

#if defined(__aarch64__) || defined(__i386__)

#include <kern/sched_prism/processor.h> /* processor_t, current_processor, struct processor */

/* The CPU running this code, or null before the scheduler registers it. */
static inline processor_t
vm_current_cpu(void)
{
	return current_processor();
}

/* The user address space loaded on `cpu` (its TTBR0 / CR3), or null. */
static inline struct vm_address_space *
vm_cpu_active_space(processor_t cpu)
{
	return cpu->active_space;
}

/* Record the user address space now loaded on `cpu`. */
static inline void
vm_cpu_set_active_space(processor_t cpu, struct vm_address_space *space)
{
	cpu->active_space = space;
}

/* The logical id of `cpu` (its bit in an nxu_cpuset_t). */
static inline uint32_t
vm_cpu_id(processor_t cpu)
{
	return cpu->cpu_id;
}

#else /* no sched_prism scheduler (nascent single-CPU x86_64) */

typedef struct processor *processor_t;
struct vm_address_space;

/*
 * No scheduler and one CPU: there is no per-CPU object, so current_cpu() is
 * always null and the VM core tracks the active space in its g_boot_active_space
 * fallback (the same path the i386 self-test uses before registration). The
 * per-CPU accessors below are consequently never called; they are present only
 * so the shared core links, and must be revisited when this port gains the
 * scheduler.
 */
static inline processor_t
vm_current_cpu(void)
{
	return 0;
}

static inline struct vm_address_space *
vm_cpu_active_space(processor_t cpu)
{
	(void)cpu;
	return 0;
}

static inline void
vm_cpu_set_active_space(processor_t cpu, struct vm_address_space *space)
{
	(void)cpu;
	(void)space;
}

static inline uint32_t
vm_cpu_id(processor_t cpu)
{
	(void)cpu;
	return 0U;
}

#endif

#endif /* NXU_VM_VM_CPU_H */
