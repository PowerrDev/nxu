/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/clock/clock.c
 *
 * See kern/clock/clock.h.
 */

#include <kern/clock/clock.h>

#include <kern/console/console.h>

#include <stdbool.h>
#include <stdint.h>

static bool g_clock_ready;

bool
clock_init(void)
{
	kprintf("initializing monotonic clock\n");

	uint64_t frequency = timer_get_frequency();

	if (frequency == 0ULL) {
		kprintf("the timebase counter is not calibrated: no monotonic clock\n");
		return false;
	}

	uint64_t first = clock_uptime_us();
	uint64_t second = clock_uptime_us();

	if (second < first) {
		kprintf("the timebase counter went backwards (%llu us, then %llu us): no monotonic clock\n",
				(unsigned long long)first, (unsigned long long)second);
		return false;
	}

	/* kprintf has no field widths: the kHz remainder is padded by hand. */
	uint32_t khz = (uint32_t)(frequency / 1000ULL % 1000ULL);

	kprintf("timebase %llu.%c%c%c MHz, %llu us since the counter started\n",
			(unsigned long long)(frequency / 1000000ULL), (char)('0' + khz / 100U), (char)('0' + khz / 10U % 10U),
			(char)('0' + khz % 10U), (unsigned long long)second);

	__atomic_store_n(&g_clock_ready, true, __ATOMIC_RELEASE);
	kprintf("monotonic clock initialized\n");
	return true;
}

bool
clock_is_ready(void)
{
	return __atomic_load_n(&g_clock_ready, __ATOMIC_ACQUIRE);
}
