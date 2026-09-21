/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/machine/smp.h
 *
 * Architecture dispatch for the per-CPU primitives machine-independent code
 * needs:
 *
 *   machine_cpu_local()       this CPU's per-CPU object (0 before it is set)
 *   machine_cpu_local_set(p)  install it
 *   machine_cpu_id()          this CPU's logical id (0 before any is set)
 *
 * The per-CPU object is `struct processor` (kern/sched_prism/processor.h);
 * the contract with the architecture code is only that its first word is the
 * logical CPU id, so machine_cpu_id() needs no knowledge of the structure.
 * Logical ids are dense and 0 is the boot CPU; they are not MPIDR values.
 */
#ifndef NXU_KERN_MACHINE_SMP_H
#define NXU_KERN_MACHINE_SMP_H

#if defined(__aarch64__)
#include <kern/arm64/smp.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <kern/i386/smp.h>
#else
#error "machine/smp.h: unsupported target architecture"
#endif

#endif
