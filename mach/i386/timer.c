/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/timer.c
 *
 * Timekeeping for x86, in the shape of mach/arm64/timer.c.
 *
 * The free-running counter is the TSC. Its frequency is not architecturally
 * enumerable everywhere, so it is measured by timing a fixed PIT channel 2
 * countdown (the PIT always runs at 1193182 Hz).
 *
 * The periodic tick is PIT channel 0 in mode 2 (rate generator), which
 * raises IRQ0 every divisor input clocks and reloads itself. Like the arm64
 * physical timer it only produces interrupts; counting them and charging the
 * scheduler is done from the IRQ0 handler (see irq.c).
 */

#include <mach/i386/timer.h>

#include <mach/i386/io.h>
#include <mach/i386/pic.h>

#include <mach/machine/machine_routines.h>

#include <stdint.h>

#define PIT_FREQUENCY_HZ 1193182ULL
#define PIT_CHANNEL2_DATA 0x42U
#define PIT_COMMAND 0x43U
#define PIT_GATE_PORT 0x61U

#define PIT_CHANNEL0_DATA 0x40U

/* Channel 0, lobyte/hibyte access, mode 2 (rate generator), binary. */
#define PIT_CMD_CHANNEL0_RATE 0x34U

#define PIT_CALIBRATION_COUNT 11932U

/* Mode 2 needs a reload of at least 2; a reload of 0 means 65536. */
#define PIT_MIN_DIVISOR 2U
#define PIT_MAX_DIVISOR 65536U

static uint64_t g_tsc_frequency;
static volatile uint64_t g_timer_interrupt_count;
static uint32_t g_timer_interrupt_rate;

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

	uint64_t wait_ticks =
		milliseconds / 1000ULL * g_tsc_frequency +
		milliseconds % 1000ULL * g_tsc_frequency / 1000ULL;
	uint64_t start = timer_rdtsc();

	while ((timer_rdtsc() - start) < wait_ticks) {
		__asm__ volatile("pause");
	}
}

void timer_start_periodic(uint32_t frequency_hz)
{
	if (frequency_hz == 0U) return;

	uint64_t divisor = (PIT_FREQUENCY_HZ + frequency_hz / 2U) / frequency_hz;

	if (divisor < PIT_MIN_DIVISOR) divisor = PIT_MIN_DIVISOR;
	if (divisor > PIT_MAX_DIVISOR) divisor = PIT_MAX_DIVISOR;

	uint64_t state = ml_irq_save();

	g_timer_interrupt_count = 0ULL;
	g_timer_interrupt_rate = frequency_hz;

	/* A divisor of 65536 is written as 0. */
	outb(PIT_COMMAND, PIT_CMD_CHANNEL0_RATE);
	outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFFU));
	outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8U) & 0xFFU));

	/* The PIT is only heard once its line is open at the controller. */
	pic_unmask(0U);

	ml_irq_restore(state);
}

void timer_handle_interrupt(void)
{
	/* The PIT reloads itself, so unlike arm64 there is nothing to rearm. */
	g_timer_interrupt_count++;
}

uint64_t timer_get_interrupt_count(void)
{
	/* A 64-bit load is two instructions here; keep the handler out of it. */
	uint64_t state = ml_irq_save();
	uint64_t count = g_timer_interrupt_count;

	ml_irq_restore(state);
	return count;
}

uint32_t timer_get_interrupt_rate(void)
{
	return g_timer_interrupt_rate;
}
