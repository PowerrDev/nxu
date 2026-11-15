/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/clock/clock.h
 *
 * The monotonic clock every kernel deadline is expressed on: microseconds
 * since the machine's free-running counter started (the TSC on x86, the
 * generic timer's counter on arm64), never going backwards and unaffected
 * by the wall-clock time. Timer calls (kern/clock/timer_call.h), the event
 * timer (kern/clock/etimer.h), timed waits and the scheduler's deadlines are
 * all on this clock.
 *
 * It is the architecture's timer_get_microseconds() (<kern/machine/timer.h>)
 * under a name that says what it is for; clock_init() only checks and
 * reports the timebase it rests on.
 */

#ifndef NXU_KERN_CLOCK_CLOCK_H
#define NXU_KERN_CLOCK_CLOCK_H

#include <kern/machine/timer.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * clock_init
 *
 * Check that the counter under the clock is calibrated and report its
 * frequency. Once, on the boot CPU, after the architecture has calibrated
 * its counter. Returns false (and says so) if the clock cannot be used:
 * nothing time-based would work.
 */
bool clock_init(void);

/* Whether clock_init succeeded. */
bool clock_is_ready(void);

/* Microseconds on the monotonic clock. Any context, any CPU. */
static inline uint64_t
clock_uptime_us(void)
{
	return timer_get_microseconds();
}

#endif
