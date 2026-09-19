/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/interrupts_test.c
 *
 * i386_init_interrupts_selftest(), run by "test=interrupts". It turns
 * interrupts on and checks, in order:
 *
 *   pic       mask bookkeeping and the cascade line
 *   tick      IRQ0 rate against the TSC, and a restart at another rate
 *   delay     timer_delay_ms against the (independent) PIT tick count
 *   dispatch  irq_register / irq_dispatch through software interrupts,
 *             chained handlers, duplicate and full-chain refusal
 *   spurious  vectors 39 and 47 with nothing in service are ignored
 *   mask      a masked line stops a real interrupt (IRQ0)
 *   cascade   a slave line (RTC, IRQ8) is delivered, EOI'd and maskable
 *
 * Interrupts are enabled only inside wait_with_interrupts() and are clear
 * again on return, so the state the test leaves behind is the state the
 * phase left: IF clear, IRQ0 ticking at I386_TIMER_HZ.
 */

#include <mach/i386/boot_info.h>
#include <mach/i386/io.h>
#include <mach/i386/irq.h>
#include <mach/i386/pic.h>
#include <mach/i386/timer.h>
#include <mach/i386/trap.h>

#include <mach/machine/machine_routines.h>

#include <kern/console/console.h>
#include <kern/irq/irq.h>

#include <stdbool.h>
#include <stdint.h>

#define TEST_SOFT_LINE 5U
#define TEST_SOFT_VECTOR 37U /* T_IRQ_BASE + TEST_SOFT_LINE */
#define TEST_SPURIOUS_MASTER_VECTOR 39U
#define TEST_SPURIOUS_SLAVE_VECTOR 47U

#define TEST_CHAIN_MAX 32U

#define CMOS_INDEX 0x70U
#define CMOS_DATA 0x71U
#define CMOS_REG_A 0x0AU
#define CMOS_REG_B 0x0BU
#define CMOS_REG_C 0x0CU
#define CMOS_A_DIVIDER_32K 0x20U
#define CMOS_A_RATE_1024HZ 0x06U
#define CMOS_B_PERIODIC_IRQ 0x40U
#define CMOS_C_PERIODIC 0x40U

typedef struct {
	volatile uint32_t hits;
	volatile uint32_t last_intid;
	volatile uint32_t sequence;
	volatile uint32_t seen_if_set;
} probe_t;

static uint32_t g_failures;
static volatile uint32_t g_sequence;

static void check(bool ok, const char *what)
{
	if (!ok) g_failures++;

	kprintf("i386_init_interrupts_selftest: %s: %s\n", ok ? "ok" : "FAIL", what);
}

static bool near(uint64_t value, uint64_t expected, uint64_t slack)
{
	return value + slack >= expected && value <= expected + slack;
}

static void probe_handler(uint32_t intid, void *context)
{
	probe_t *probe = context;

	probe->hits++;
	probe->last_intid = intid;
	probe->sequence = ++g_sequence;

	if ((i386_read_flags() & I386_EFLAGS_IF) != 0ULL) probe->seen_if_set++;
}

/* Let interrupts in for a while, on the TSC so it works while they are off. */
static void wait_with_interrupts(uint64_t milliseconds)
{
	ml_irq_enable();
	timer_delay_ms(milliseconds);
	ml_irq_disable();
}

static uint8_t cmos_read(uint8_t reg)
{
	outb(CMOS_INDEX, reg);
	return inb(CMOS_DATA);
}

static void cmos_write(uint8_t reg, uint8_t value)
{
	outb(CMOS_INDEX, reg);
	outb(CMOS_DATA, value);
}

/* IRQ8 handler: reading register C is what re-arms the RTC interrupt. */
static void rtc_handler(uint32_t intid, void *context)
{
	if ((cmos_read(CMOS_REG_C) & CMOS_C_PERIODIC) != 0U) probe_handler(intid, context);
}

static void test_pic(void)
{
	bool consistent = true;
	uint16_t imr = pic_read_imr();

	for (uint32_t line = 0U; line < PIC_LINE_COUNT; line++) {
		if (line == PIC_CASCADE_LINE) continue;
		if (((imr >> line) & 1U) != (pic_is_masked(line) ? 1U : 0U)) consistent = false;
	}

	check(consistent, "hardware mask registers agree with the driver");
	check(!pic_is_masked(I386_TIMER_IRQ), "IRQ0 (timer) is unmasked");
	check(pic_read_isr() == 0U, "no interrupt is in service");

	/* Roll a master and a slave line through mask and unmask. */
	pic_unmask(3U);
	check((pic_read_imr() & (1U << 3U)) == 0U && !pic_is_masked(3U), "unmask of IRQ3 reaches the master");
	pic_mask(3U);
	check((pic_read_imr() & (1U << 3U)) != 0U && pic_is_masked(3U), "mask of IRQ3 reaches the master");

	bool slave_idle = (pic_read_imr() & 0xFF00U) == 0xFF00U;

	pic_unmask(11U);
	check((pic_read_imr() & (1U << 11U)) == 0U, "unmask of IRQ11 reaches the slave");
	check((pic_read_imr() & (1U << PIC_CASCADE_LINE)) == 0U, "unmasking a slave line opens the cascade");
	pic_mask(11U);

	if (slave_idle) check((pic_read_imr() & (1U << PIC_CASCADE_LINE)) != 0U, "masking the last slave line closes the cascade");
}

/*
 * QEMU's PIT model can drop ticks when the host is busy: it computes the
 * output level when its host timer finally fires, and a late timer misses
 * the one-clock pulse of mode 2. Real hardware does not do that, so a run
 * is judged one-sided per window and two-sided over the best window: no
 * window may see more ticks than its length allows (the rate is not too
 * fast, a delay is not too long), and at least one window must see about as
 * many as it should (the rate is not too slow, a delay is not too short).
 */
typedef struct {
	bool none_too_many;
	bool one_on_target;
	uint64_t shortest_us;
	uint64_t best_ticks;
	uint64_t best_expected;
} window_result_t;

/* Wait @windows times @delay_ms with interrupts on and compare ticks to time. */
static window_result_t measure_windows(uint32_t windows, uint64_t delay_ms)
{
	window_result_t result = { true, false, ~0ULL, 0ULL, 0ULL };

	for (uint32_t window = 0U; window < windows; window++) {
		uint64_t count0 = timer_get_interrupt_count();
		uint64_t time0 = timer_get_microseconds();

		wait_with_interrupts(delay_ms);

		uint64_t elapsed = timer_get_microseconds() - time0;
		uint64_t ticks = timer_get_interrupt_count() - count0;
		uint64_t expected = elapsed * timer_get_interrupt_rate() / 1000000ULL;
		uint64_t slack = 2ULL + expected / 10ULL;

		if (elapsed < result.shortest_us) result.shortest_us = elapsed;
		if (ticks > expected + slack) result.none_too_many = false;

		if (near(ticks, expected, slack) && ticks >= result.best_ticks) {
			result.one_on_target = true;
			result.best_ticks = ticks;
			result.best_expected = expected;
		}
	}

	return result;
}

static void test_tick_rate(void)
{
	window_result_t result = measure_windows(5U, 100ULL);

	kprintf(
		"i386_init_interrupts_selftest: best window %llu ticks, expected about %llu, at %u Hz\n",
		(unsigned long long)result.best_ticks,
		(unsigned long long)result.best_expected,
		timer_get_interrupt_rate()
	);

	check(timer_get_interrupt_rate() == I386_TIMER_HZ, "timer_get_interrupt_rate reports the configured rate");
	check(timer_get_interrupt_count() > 0ULL, "the IRQ0 tick count advances");
	check(result.none_too_many, "IRQ0 never ticks faster than the requested rate");
	check(result.one_on_target, "IRQ0 ticks at the requested rate");
}

static void test_delay_and_restart(void)
{
	timer_start_periodic(200U);

	check(timer_get_interrupt_rate() == 200U, "timer_start_periodic changes the reported rate");
	check(timer_get_interrupt_count() == 0ULL, "timer_start_periodic resets the interrupt count");

	/* 100 ms of timer_delay_ms against the PIT is about 20 ticks at 200 Hz. */
	window_result_t result = measure_windows(5U, 100ULL);

	kprintf(
		"i386_init_interrupts_selftest: timer_delay_ms(100) x5: shortest %llu us, best window %llu ticks (expected about %llu) at 200 Hz\n",
		(unsigned long long)result.shortest_us,
		(unsigned long long)result.best_ticks,
		(unsigned long long)result.best_expected
	);

	check(result.shortest_us >= 100000ULL, "timer_delay_ms waits at least the requested time");
	check(result.none_too_many, "timer_delay_ms does not wait longer than requested");
	check(result.one_on_target, "timer_delay_ms(100) spans 100 ms of PIT ticks");

	/* Without interrupts the delay still expires. */
	uint64_t start = timer_get_microseconds();

	timer_delay_ms(20ULL);
	check(timer_get_microseconds() - start >= 20000ULL, "timer_delay_ms works with interrupts disabled");

	start = timer_get_microseconds();
	timer_delay_ms(0ULL);
	check(timer_get_microseconds() - start < 5000ULL, "timer_delay_ms(0) returns at once");

	timer_start_periodic(I386_TIMER_HZ);
	check(timer_get_interrupt_rate() == I386_TIMER_HZ, "the boot rate is restored");
}

static void test_dispatch(void)
{
	probe_t a = { 0U, 0U, 0U, 0U };
	probe_t b = { 0U, 0U, 0U, 0U };
	probe_t c = { 0U, 0U, 0U, 0U };

	check(!irq_dispatch(TEST_SOFT_LINE), "irq_dispatch on an empty line reports unhandled");
	check(!irq_register(PIC_LINE_COUNT, probe_handler, &a), "irq_register refuses a line above 15");
	check(!irq_register(TEST_SOFT_LINE, 0, &a), "irq_register refuses a null handler");

	check(irq_register(TEST_SOFT_LINE, probe_handler, &a), "irq_register accepts the first handler");
	check(!irq_register(TEST_SOFT_LINE, probe_handler, &a), "irq_register refuses the same handler and context twice");

	__asm__ volatile("int %0" : : "i"(TEST_SOFT_VECTOR) : "memory");
	check(a.hits == 1U && a.last_intid == TEST_SOFT_LINE, "int $37 reaches the handler with intid 5");
	check(a.seen_if_set == 0U, "handlers run with IF clear");

	check(irq_register(TEST_SOFT_LINE, probe_handler, &b), "a second handler chains on the same line");
	check(irq_register(TEST_SOFT_LINE, probe_handler, &c), "a third handler chains on the same line");

	__asm__ volatile("int %0" : : "i"(TEST_SOFT_VECTOR) : "memory");
	check(a.hits == 2U && b.hits == 1U && c.hits == 1U, "every chained handler runs");
	check(a.sequence < b.sequence && b.sequence < c.sequence, "chained handlers run in registration order");
	check(irq_dispatch(TEST_SOFT_LINE) && a.hits == 3U && c.hits == 2U, "irq_dispatch runs the chain and reports handled");

	check(!irq_unregister(TEST_SOFT_LINE, probe_handler, &g_failures), "irq_unregister needs a matching context");
	check(irq_unregister(TEST_SOFT_LINE, probe_handler, &b), "irq_unregister removes one handler");

	__asm__ volatile("int %0" : : "i"(TEST_SOFT_VECTOR) : "memory");
	check(a.hits == 4U && b.hits == 2U && c.hits == 3U, "the remaining handlers still run");

	check(irq_unregister(TEST_SOFT_LINE, probe_handler, &a), "irq_unregister removes the first handler");
	check(irq_unregister(TEST_SOFT_LINE, probe_handler, &c), "irq_unregister removes the last handler");
	check(!irq_dispatch(TEST_SOFT_LINE), "the line is empty again");

	/* Fill a chain until it refuses, prove all of it runs, then empty it. */
	static probe_t chain[TEST_CHAIN_MAX];
	uint32_t registered = 0U;

	while (registered < TEST_CHAIN_MAX && irq_register(TEST_SOFT_LINE, probe_handler, &chain[registered])) registered++;

	check(registered >= 4U && registered < TEST_CHAIN_MAX, "a line takes several handlers and a full chain is refused");

	__asm__ volatile("int %0" : : "i"(TEST_SOFT_VECTOR) : "memory");

	bool all_ran = true;

	for (uint32_t index = 0U; index < registered; index++) {
		if (chain[index].hits != 1U) all_ran = false;
	}

	check(all_ran, "every handler of a full chain runs");

	bool all_removed = true;

	for (uint32_t index = 0U; index < registered; index++) {
		if (!irq_unregister(TEST_SOFT_LINE, probe_handler, &chain[index])) all_removed = false;
	}

	check(all_removed && !irq_dispatch(TEST_SOFT_LINE), "a full chain unregisters cleanly");
}

static void test_spurious(void)
{
	probe_t master = { 0U, 0U, 0U, 0U };
	probe_t slave = { 0U, 0U, 0U, 0U };
	uint32_t before = pic_spurious_count();

	check(irq_register(7U, probe_handler, &master), "handler registered on IRQ7");
	check(irq_register(15U, probe_handler, &slave), "handler registered on IRQ15");

	/* Nothing is in service on either controller, so both look spurious. */
	__asm__ volatile("int %0" : : "i"(TEST_SPURIOUS_MASTER_VECTOR) : "memory");
	__asm__ volatile("int %0" : : "i"(TEST_SPURIOUS_SLAVE_VECTOR) : "memory");

	check(pic_spurious_count() == before + 2U, "spurious IRQ7 and IRQ15 are recognised");
	check(master.hits == 0U && slave.hits == 0U, "no handler runs for a spurious interrupt");
	check(pic_read_isr() == 0U, "a spurious interrupt leaves nothing in service");

	check(irq_unregister(7U, probe_handler, &master), "IRQ7 handler removed");
	check(irq_unregister(15U, probe_handler, &slave), "IRQ15 handler removed");
}

static void test_mask(void)
{
	probe_t probe = { 0U, 0U, 0U, 0U };

	check(irq_register(I386_TIMER_IRQ, probe_handler, &probe), "a handler chains behind the timer on IRQ0");

	uint64_t ticks0 = timer_get_interrupt_count();

	wait_with_interrupts(100ULL);

	uint64_t ticks = timer_get_interrupt_count() - ticks0;

	check(probe.hits > 0U, "a real PIT interrupt reaches a handler registered with irq_register");
	check(probe.last_intid == I386_TIMER_IRQ && probe.seen_if_set == 0U, "the real interrupt arrives as IRQ0 with IF clear");
	check(probe.hits == ticks, "the tick handler and the chained handler run once per interrupt");

	pic_mask(I386_TIMER_IRQ);

	uint32_t hits = probe.hits;
	uint64_t count = timer_get_interrupt_count();

	wait_with_interrupts(100ULL);
	check(probe.hits == hits && timer_get_interrupt_count() == count, "a masked line delivers nothing");

	pic_unmask(I386_TIMER_IRQ);
	wait_with_interrupts(100ULL);
	check(probe.hits > hits && timer_get_interrupt_count() > count, "an unmasked line delivers again");

	check(irq_unregister(I386_TIMER_IRQ, probe_handler, &probe), "the IRQ0 test handler is removed");
}

static void test_cascade(void)
{
	probe_t rtc = { 0U, 0U, 0U, 0U };
	uint8_t saved_a = cmos_read(CMOS_REG_A);
	uint8_t saved_b = cmos_read(CMOS_REG_B);

	check(irq_register(I386_RTC_IRQ, rtc_handler, &rtc), "handler registered on IRQ8");

	cmos_write(CMOS_REG_A, (uint8_t)(CMOS_A_DIVIDER_32K | CMOS_A_RATE_1024HZ));
	(void)cmos_read(CMOS_REG_C);
	cmos_write(CMOS_REG_B, (uint8_t)(saved_b | CMOS_B_PERIODIC_IRQ));

	pic_unmask(I386_RTC_IRQ);
	wait_with_interrupts(50ULL);

	kprintf("i386_init_interrupts_selftest: %u RTC interrupts in 50 ms at 1024 Hz\n", rtc.hits);
	check(rtc.hits >= 10U, "a slave line (IRQ8) is delivered repeatedly, so the cascade EOI works");
	check(rtc.last_intid == I386_RTC_IRQ && rtc.seen_if_set == 0U, "the slave interrupt arrives as IRQ8 with IF clear");

	pic_mask(I386_RTC_IRQ);

	uint32_t hits = rtc.hits;

	wait_with_interrupts(30ULL);
	check(rtc.hits == hits, "a masked slave line delivers nothing");

	cmos_write(CMOS_REG_B, saved_b);
	cmos_write(CMOS_REG_A, saved_a);
	(void)cmos_read(CMOS_REG_C);

	check(irq_unregister(I386_RTC_IRQ, rtc_handler, &rtc), "IRQ8 handler removed");
}

bool i386_init_interrupts_selftest(const i386_boot_info_t *boot)
{
	(void)boot;

	uint64_t flags = ml_irq_save();

	g_failures = 0U;

	check((flags & I386_EFLAGS_IF) == 0ULL, "the phase leaves interrupts disabled");

	test_pic();
	test_tick_rate();
	test_delay_and_restart();
	test_dispatch();
	test_spurious();
	test_mask();
	test_cascade();

	check(pic_read_isr() == 0U, "every interrupt that was taken has been acknowledged");
	check(i386_irq_unhandled_count() == 0U, "no hardware interrupt was left without a handler");
	check(!pic_is_masked(I386_TIMER_IRQ) && timer_get_interrupt_rate() == I386_TIMER_HZ, "the boot timer configuration is intact");

	ml_irq_restore(flags);
	check((i386_read_flags() & I386_EFLAGS_IF) == 0ULL, "interrupts are disabled again on return");

	kprintf("i386_init_interrupts_selftest: %u failure(s)\n", g_failures);
	return g_failures == 0U;
}
