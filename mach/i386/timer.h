/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/timer.h
 *
 * x86 flavour of the counter API in <mach/machine/timer.h>. The free-running
 * counter is the TSC, calibrated once at boot against PIT channel 2.
 */

#ifndef NXU_MACH_I386_TIMER_H
#define NXU_MACH_I386_TIMER_H

#include <stdint.h>

/* Measure the TSC against the PIT. Returns 0 if the counter is unusable. */
uint64_t timer_calibrate(void);

uint64_t timer_get_frequency(void);
uint64_t timer_get_ticks(void);
uint64_t timer_ticks_to_microseconds(uint64_t ticks);
uint64_t timer_get_microseconds(void);
void timer_delay_ms(uint64_t milliseconds);

#endif
