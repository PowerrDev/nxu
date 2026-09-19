#include <mach/arm64/timer.h>

#include <stdint.h>

static volatile uint64_t g_timer_interrupt_count;
static uint32_t g_timer_interval_ticks;
static uint32_t g_timer_interrupt_rate;

/*
 * Read how many times the system counter increments per second.
 */
static inline uint64_t arm64_read_counter_frequency(void)
{
	uint64_t value;

	__asm__ volatile(
		"mrs %0, CNTFRQ_EL0"
		: "=r"(value)
	);

	return value;
}

/*
 * Read the current physical system-counter value.
 */
static inline uint64_t arm64_read_physical_counter(void)
{
	uint64_t value;

	__asm__ volatile(
		"isb\n"
		"mrs %0, CNTPCT_EL0"
		: "=r"(value)
	);

	return value;
}

uint64_t timer_get_frequency(void)
{
	return arm64_read_counter_frequency();
}

uint64_t timer_get_ticks(void)
{
	return arm64_read_physical_counter();
}

uint64_t timer_ticks_to_microseconds(uint64_t ticks)
{
	uint64_t frequency = timer_get_frequency();

	if (frequency == 0) {
		return 0;
	}

	uint64_t seconds = ticks / frequency;
	uint64_t remaining_ticks = ticks % frequency;

	return seconds * 1000000UL
		+ remaining_ticks * 1000000UL / frequency;
}

uint64_t timer_get_microseconds(void)
{
	return timer_ticks_to_microseconds(timer_get_ticks());
}

void timer_delay_ms(uint64_t milliseconds)
{
	uint64_t frequency = timer_get_frequency();

	uint64_t whole_seconds = milliseconds / 1000UL;
	uint64_t remaining_ms = milliseconds % 1000UL;

	uint64_t wait_ticks =
		whole_seconds * frequency
		+ remaining_ms * frequency / 1000UL;

	uint64_t start = timer_get_ticks();

	while ((timer_get_ticks() - start) < wait_ticks) {
		__asm__ volatile("yield");
	}
}

static inline void arm64_write_physical_timer_value(uint32_t value)
{
	__asm__ volatile(
		"msr CNTP_TVAL_EL0, %0"
		:
		: "r"((uint64_t)value)
	);
}

static inline void arm64_write_physical_timer_control(uint32_t value)
{
	__asm__ volatile(
		"msr CNTP_CTL_EL0, %0\n"
		"isb"
		:
		: "r"((uint64_t)value)
		: "memory"
	);
}

void timer_start_periodic(uint32_t frequency_hz)
{
	uint64_t counter_frequency = timer_get_frequency();

	if (frequency_hz == 0U || counter_frequency == 0U) {
		return;
	}

	uint64_t interval = counter_frequency / frequency_hz;

	if (interval == 0U) {
		interval = 1U;
	}

	/*
	 * CNTP_TVAL_EL0 acts as a signed 32-bit timer value.
	 * Keep our interval inside its positive range.
	 */
	if (interval > 0x7FFFFFFFUL) {
		interval = 0x7FFFFFFFUL;
	}

	g_timer_interrupt_count = 0;
	g_timer_interrupt_rate = frequency_hz;
	g_timer_interval_ticks = (uint32_t)interval;

	/*
	 * The timer will expire after this many physical-counter ticks.
	 */
	arm64_write_physical_timer_value(g_timer_interval_ticks);

	/*
	 * CNTP_CTL_EL0:
	 *
	 * bit 0 ENABLE = 1
	 * bit 1 IMASK  = 0
	 *
	 * Therefore, 1 enables and unmasks the timer.
	 */
	arm64_write_physical_timer_control(1U);
}

void timer_handle_interrupt(void)
{
	/*
	 * Rearm the timer first. This moves the comparison point into
	 * the future and deasserts the level-sensitive timer signal.
	 */
	arm64_write_physical_timer_value(g_timer_interval_ticks);

	g_timer_interrupt_count++;
}

uint64_t timer_get_interrupt_count(void)
{
	return g_timer_interrupt_count;
}

uint32_t timer_get_interrupt_rate(void)
{
	return g_timer_interrupt_rate;
}
