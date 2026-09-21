#ifndef NXU_TIMER_H
#define NXU_TIMER_H

/* The EL1 physical timer's private peripheral interrupt: the same INTID on every CPU. */
#define PHYSICAL_TIMER_INTID 30U

#include <stdint.h>

/**
 * timer_get_frequency - System counter increment rate.
 *
 * Reads CNTFRQ_EL0, which firmware programs to describe the counter rate.
 * The kernel treats it as authoritative and does not derive it.
 *
 * Return: ticks per second, or 0 if firmware left the register unset.
 */
uint64_t timer_get_frequency(void);

/**
 * timer_get_ticks - Current physical system counter value.
 *
 * Reads CNTPCT_EL0, preceded by an ISB so the read is not reordered ahead of
 * surrounding instructions. The counter is free-running and monotonic.
 *
 * Safe from any context.
 *
 * Return: the raw 64-bit counter value.
 */
uint64_t timer_get_ticks(void);

/**
 * timer_ticks_to_microseconds - Convert counter ticks to microseconds.
 * @ticks: A counter value or difference.
 *
 * Splits the conversion into whole seconds plus a remainder so the
 * intermediate multiplication cannot overflow 64 bits.
 *
 * Return: the equivalent microseconds, or 0 if CNTFRQ_EL0 is 0.
 */
uint64_t timer_ticks_to_microseconds(uint64_t ticks);

/**
 * timer_get_microseconds - Elapsed microseconds since the counter started.
 *
 * Return: the current counter value converted to microseconds, or 0 if
 * CNTFRQ_EL0 is 0.
 */
uint64_t timer_get_microseconds(void);

/**
 * timer_get_interrupt_count - Number of timer interrupts delivered.
 *
 * Counts delivered interrupts, not elapsed time. A tick lost because
 * interrupts were masked makes this lag real time permanently; use
 * timer_get_microseconds() for a wall-clock reading.
 *
 * Safe from any context: the counter is volatile and the handler is its only
 * writer.
 *
 * Return: the interrupt count since timer_start_periodic() was last called.
 */
uint64_t timer_get_interrupt_count(void);

/**
 * timer_get_interrupt_rate - Configured periodic rate.
 *
 * Return: the frequency in Hz passed to timer_start_periodic(), or 0 if the
 * timer has not been started.
 */
uint32_t timer_get_interrupt_rate(void);

/**
 * timer_delay_ms - Busy-wait for a number of milliseconds.
 * @milliseconds: How long to spin.
 *
 * Spins on CNTPCT_EL0 with a YIELD hint. Does not sleep, does not yield to
 * anything, and does not require interrupts to be enabled.
 */
void timer_delay_ms(uint64_t milliseconds);

/**
 * timer_start_periodic - Arm the physical timer at a fixed rate.
 * @frequency_hz: Desired interrupts per second.
 *
 * Computes the interval as CNTFRQ_EL0 / @frequency_hz, clamped to the
 * positive range of the signed 32-bit CNTP_TVAL_EL0, then enables the timer
 * with its interrupt unmasked. Resets the interrupt count to zero.
 *
 * Arming the timer only makes the interrupt pending; it is taken once the
 * PPI is enabled at the GIC and PSTATE.I is cleared.
 *
 * The division truncates, so an inexact rate produces slightly more
 * interrupts per second than requested.
 *
 * Silently does nothing if @frequency_hz or CNTFRQ_EL0 is 0. Must not be
 * called from interrupt context: it rewrites state the handler reads.
 */
void timer_start_periodic(uint32_t frequency_hz);

/**
 * timer_start_local - Arm the calling CPU's physical timer.
 *
 * The generic timer is banked per CPU: each CPU has its own CNTP_* registers
 * and its own PPI. Uses the rate timer_start_periodic() set up on the boot
 * CPU, which must have run first. The CPU must also enable the timer PPI in
 * its own redistributor (gic_enable_ppi()).
 */
void timer_start_local(void);

/**
 * timer_handle_interrupt - Service a timer interrupt.
 *
 * Rearms the timer before incrementing the count. The order is required:
 * the interrupt is level-sensitive, so rewriting CNTP_TVAL_EL0 is what
 * deasserts the signal. Acknowledging the interrupt without rearming first
 * would leave it immediately pending again.
 *
 * Interrupt-context function. Called only from the IRQ dispatcher.
 */
void timer_handle_interrupt(void);

#endif
