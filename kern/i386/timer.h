/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/timer.h
 *
 * x86 flavour of the timer API in <kern/machine/timer.h>, with the same
 * semantics as <kern/arm64/timer.h>. The free-running counter is the TSC,
 * calibrated once at boot against PIT channel 2; the periodic interrupt is
 * PIT channel 0 on IRQ0.
 */

#ifndef NXU_KERN_I386_TIMER_H
#define NXU_KERN_I386_TIMER_H

#include <stdint.h>

/* Measure the TSC against the PIT. Returns 0 if the counter is unusable. */
uint64_t timer_calibrate(void);

/* TSC increments per second, or 0 before timer_calibrate() / if unusable. */
uint64_t timer_get_frequency(void);

/* Current TSC value: free-running and monotonic. Safe from any context. */
uint64_t timer_get_ticks(void);

/* Convert TSC ticks (or a difference) to microseconds; 0 if uncalibrated. */
uint64_t timer_ticks_to_microseconds(uint64_t ticks);

/* Microseconds since the TSC started counting; 0 if uncalibrated. */
uint64_t timer_get_microseconds(void);

/*
 * timer_get_interrupt_count - Number of timer interrupts delivered.
 *
 * Counts delivered interrupts, not elapsed time: a tick lost while IRQ0 was
 * masked or interrupts were disabled makes this lag real time permanently.
 * Use timer_get_microseconds() for a wall-clock reading. Safe from any
 * context.
 *
 * Return: the count since timer_start_periodic() was last called.
 */
uint64_t timer_get_interrupt_count(void);

/* Frequency in Hz passed to timer_start_periodic(), or 0 if never started. */
uint32_t timer_get_interrupt_rate(void);

/*
 * timer_delay_ms - Busy-wait for a number of milliseconds on the TSC.
 *
 * Does not sleep or yield, and does not need interrupts enabled. Returns
 * immediately if the TSC is uncalibrated.
 */
void timer_delay_ms(uint64_t milliseconds);

/*
 * timer_start_periodic - Program PIT channel 0 to fire at a fixed rate.
 * @frequency_hz: Desired interrupts per second.
 *
 * Divides the 1193182 Hz PIT clock by the rounded ratio, clamped to what the
 * 16-bit reload supports (so rates below about 19 Hz run at 18.2 Hz), opens
 * IRQ0 at the PIC, and resets the interrupt count to zero. May be called
 * again to change the rate. Does nothing if @frequency_hz is 0.
 *
 * The interrupt is taken once IF is set. Must not be called from interrupt
 * context: it rewrites state the handler reads.
 */
void timer_start_periodic(uint32_t frequency_hz);

/*
 * timer_handle_interrupt - Account one timer interrupt.
 *
 * Interrupt-context function, called from the IRQ0 handler (irq.c), which
 * also charges the scheduler tick. The PIT reloads itself, so there is
 * nothing to rearm.
 */
void timer_handle_interrupt(void);

#endif
