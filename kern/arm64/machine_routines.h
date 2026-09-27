/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/arm64/machine_routines.h
 *
 * arm64 implementation of <kern/machine/machine_routines.h>. Interrupt
 * state is the full DAIF value, so a restore reinstates every mask bit
 * exactly as it was.
 */

#ifndef NXU_KERN_ARM64_MACHINE_ROUTINES_H
#define NXU_KERN_ARM64_MACHINE_ROUTINES_H

#include <kern/arm64/system.h>

#include <stdbool.h>
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

/* Whether IRQs are unmasked on this CPU right now (DAIF.I clear). */
static inline bool ml_irq_enabled(void)
{
	uint64_t daif;

	__asm__ volatile("mrs %0, daif" : "=r"(daif));
	return (daif & (1ULL << 7U)) == 0ULL;
}

#endif
