/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/timer.c
 *
 * TSC-backed counter for x86. The TSC frequency is not architecturally
 * enumerable everywhere, so it is measured by timing a fixed PIT channel 2
 * countdown (the PIT always runs at 1193182 Hz).
 */

#include <mach/i386/timer.h>

#include <mach/i386/io.h>

#include <stdint.h>

#define PIT_FREQUENCY_HZ 1193182ULL
#define PIT_CHANNEL2_DATA 0x42U
#define PIT_COMMAND 0x43U
#define PIT_GATE_PORT 0x61U

#define PIT_CALIBRATION_COUNT 11932U

static uint64_t g_tsc_frequency;

static inline uint64_t timer_rdtsc(void)
{
	uint32_t low;
	uint32_t high;

	__asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
	return ((uint64_t)high << 32U) | low;
}

uint64_t timer_calibrate(void)
{
	uint8_t gate = inb(PIT_GATE_PORT);

	/* Gate channel 2 off and the speaker off while it is programmed. */
	outb(PIT_GATE_PORT, (uint8_t)(gate & ~0x03U));

	/* Channel 2, lobyte/hibyte, mode 0 (interrupt on terminal count). */
	outb(PIT_COMMAND, 0xB0U);
	outb(PIT_CHANNEL2_DATA, (uint8_t)(PIT_CALIBRATION_COUNT & 0xFFU));
	outb(PIT_CHANNEL2_DATA, (uint8_t)(PIT_CALIBRATION_COUNT >> 8U));

	/* Raising the gate starts the countdown. */
	outb(PIT_GATE_PORT, (uint8_t)((gate & ~0x02U) | 0x01U));

	uint64_t start = timer_rdtsc();

	while ((inb(PIT_GATE_PORT) & 0x20U) == 0U) {
		__asm__ volatile("pause");
	}

	uint64_t elapsed = timer_rdtsc() - start;

	outb(PIT_GATE_PORT, gate);

	g_tsc_frequency = elapsed * PIT_FREQUENCY_HZ / PIT_CALIBRATION_COUNT;
	return g_tsc_frequency;
}

uint64_t timer_get_frequency(void)
{
	return g_tsc_frequency;
}

uint64_t timer_get_ticks(void)
{
	return timer_rdtsc();
}

uint64_t timer_ticks_to_microseconds(uint64_t ticks)
{
	if (g_tsc_frequency == 0ULL) return 0ULL;

	return ticks / g_tsc_frequency * 1000000ULL +
		ticks % g_tsc_frequency * 1000000ULL / g_tsc_frequency;
}

uint64_t timer_get_microseconds(void)
{
	return timer_ticks_to_microseconds(timer_rdtsc());
}

void timer_delay_ms(uint64_t milliseconds)
{
	if (g_tsc_frequency == 0ULL) return;

	uint64_t deadline = timer_rdtsc() + milliseconds * (g_tsc_frequency / 1000ULL);

	while (timer_rdtsc() < deadline) {
		__asm__ volatile("pause");
	}
}
