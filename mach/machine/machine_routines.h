/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/machine/machine_routines.h
 *
 * Machine-independent view of the CPU primitives the rest of the kernel
 * needs, in the manner of XNU's <machine/machine_routines.h>. Code outside
 * mach/ includes this header and calls the ml_* routines; it never names an
 * architecture directly. The flavour is picked from the compiler's target,
 * so a single mach/i386 serves both the 32-bit (i386) and 64-bit (x86_64)
 * builds, exactly as osfmk/i386 does in XNU.
 *
 * Interrupt state is carried in a uint64_t on every architecture (DAIF on
 * arm64, EFLAGS on x86) so callers can hold it in the same local variable
 * regardless of target.
 *
 *   ml_irq_save()      mask IRQs and return the previous state
 *   ml_irq_restore(s)  put the previous state back exactly
 *   ml_irq_enable()    unmask IRQs unconditionally
 *   ml_irq_disable()   mask IRQs unconditionally
 */

#ifndef NXU_MACH_MACHINE_MACHINE_ROUTINES_H
#define NXU_MACH_MACHINE_MACHINE_ROUTINES_H

#if defined(__aarch64__)
#include <mach/arm64/machine_routines.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <mach/i386/machine_routines.h>
#else
#error "machine_routines.h: unsupported target architecture"
#endif

#endif
