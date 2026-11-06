/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/timer.c
 *
 * Early x86-64 time-counter access for the shared console. Calibration and
 * periodic interrupts belong to the later APIC/timer port.
 */

#include <kern/i386/timer.h>

#include <stdint.h>

/*
 * Routine:     timer_get_frequency
 * Purpose:
 *              Report that the TSC has not yet been calibrated on x86-64.
 */
uint64_t
timer_get_frequency(void)
{
	return 0ULL;
}

/*
 * Routine:     timer_get_ticks
 * Purpose:
 *              Read the boot CPU's free-running TSC for early console timing.
 */
uint64_t
timer_get_ticks(void)
{
	uint32_t low;
	uint32_t high;

	__asm__ volatile("rdtsc" : "=a"(low), "=d"(high));

	return ((uint64_t)high << 32U) | low;
}

/*
 * Routine:     timer_ticks_to_microseconds
 * Purpose:
 *              Convert a TSC reading (or difference) to microseconds, or 0
 *              while the TSC frequency is unknown on this early port.
 */
uint64_t
timer_ticks_to_microseconds(uint64_t ticks)
{
	uint64_t frequency = timer_get_frequency();

	if (frequency == 0ULL) return 0ULL;

	return ticks / frequency * 1000000ULL + ticks % frequency * 1000000ULL / frequency;
}

/*
 * Routine:     timer_get_microseconds
 * Purpose:
 *              Report microseconds since the TSC started, or 0 until the TSC
 *              is calibrated. Machine-independent code (e.g. the VM core's
 *              address-space quiesce) uses this for timeouts.
 */
uint64_t
timer_get_microseconds(void)
{
	return timer_ticks_to_microseconds(timer_get_ticks());
}
