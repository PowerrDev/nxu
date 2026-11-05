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
