/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/machine/cpu.h
 *
 * Architecture dispatch for the few CPU hints machine-independent code needs
 * in its spin and idle loops. Code outside mach/ calls these instead of
 * naming an instruction (`yield`, `wfe`, `wfi`, `pause`, `hlt`).
 *
 *   cpu_relax()               hint that the caller is spinning on a lock
 *   cpu_wait_for_interrupt()  stop the core until an interrupt is taken
 *   cpu_wait_for_event()      stop the core in a loop that never expects to
 *                             run again (panic and fatal-error parking)
 */

#ifndef NXU_KERN_MACHINE_CPU_H
#define NXU_KERN_MACHINE_CPU_H

#if defined(__aarch64__)
#include <kern/arm64/cpu.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <kern/i386/cpu.h>
#else
#error "machine/cpu.h: unsupported target architecture"
#endif

#endif
