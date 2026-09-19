/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/arm64/machine_routines.h
 *
 * arm64 implementation of <mach/machine/machine_routines.h>. Interrupt
 * state is the full DAIF value, so a restore reinstates every mask bit
 * exactly as it was.
 */

#ifndef NXU_MACH_ARM64_MACHINE_ROUTINES_H
#define NXU_MACH_ARM64_MACHINE_ROUTINES_H

#include <mach/arm64/system.h>

#include <stdint.h>

static inline uint64_t ml_irq_save(void)
{
	return arm64_irq_save();
}

static inline void ml_irq_restore(uint64_t state)
{
	arm64_irq_restore(state);
}

static inline void ml_irq_enable(void)
{
	arm64_irq_enable();
}

static inline void ml_irq_disable(void)
{
	arm64_irq_disable();
}

#endif
