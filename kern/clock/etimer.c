/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/clock/etimer.c
 *
 * See kern/clock/etimer.h.
 *
 * Each CPU's state is its own: only that CPU reads or writes it, always with
 * interrupts masked, so it needs no lock. The one thing other CPUs touch is
 * the CPU's timer call queue, which has its own lock (kern/clock/
 * timer_call.c) and is only read here.
 *
 * Programming policy. The hardware is reprogrammed when the earliest deadline
 * moves earlier than what it is armed for, or when nothing at all is due (it
 * is disarmed, so an idle CPU stays asleep). A deadline that moves later, or
 * goes away while another remains, leaves the hardware armed early: it fires,
 * finds nothing due, and is reprogrammed then. That trades a rare spurious
 * interrupt for not touching the device on every cancel. The scheduler's own
 * deadline is the exception, since it changes on every idle/busy transition:
 * it is always programmed exactly.
 */

#include <kern/clock/etimer.h>

#include <kern/clock/clock.h>
#include <kern/clock/timer_call.h>
#include <kern/console/console.h>
#include <kern/cpuset.h>
#include <kern/machine/machine_routines.h>
#include <kern/machine/smp.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	bool online;
	etimer_stats_t stats;
} etimer_cpu_t;

static etimer_cpu_t g_etimer_cpus[NXU_MAX_CPUS];
static const etimer_hw_t *g_etimer_hw;
static bool g_etimer_deadline_mode;
static etimer_sched_handler_t g_etimer_sched_handler;

void
etimer_init(const etimer_hw_t *hw)
{
	kprintf("initializing event timer subsystem\n");

	for (uint32_t cpu = 0U; cpu < NXU_MAX_CPUS; cpu++) {
		g_etimer_cpus[cpu] = (etimer_cpu_t){0};
		g_etimer_cpus[cpu].stats.armed_deadline = TIMER_CALL_NO_DEADLINE;
		g_etimer_cpus[cpu].stats.sched_deadline = TIMER_CALL_NO_DEADLINE;
	}

	if (hw == 0 || hw->arm == 0 || hw->disarm == 0) {
		g_etimer_hw = 0;
		__atomic_store_n(&g_etimer_deadline_mode, false, __ATOMIC_RELEASE);
		kprintf("no deadline timer: timer calls run on the boot CPU's periodic tick\n");
		return;
	}

	g_etimer_hw = hw;
	__atomic_store_n(&g_etimer_deadline_mode, true, __ATOMIC_RELEASE);
	kprintf("deadline synchronization enabled on the %s\n", hw->name);
}

bool
etimer_deadline_mode(void)
{
	return __atomic_load_n(&g_etimer_deadline_mode, __ATOMIC_ACQUIRE);
}

const char *
etimer_hw_name(void)
{
	return etimer_deadline_mode() ? g_etimer_hw->name : "periodic tick";
}

static etimer_cpu_t *
etimer_local(uint32_t *cpu_out)
{
	uint32_t cpu = machine_cpu_id();

	if (cpu >= NXU_MAX_CPUS) return 0;

	if (cpu_out != 0) *cpu_out = cpu;
	return &g_etimer_cpus[cpu];
}

static uint64_t
etimer_next_deadline(etimer_cpu_t *state, uint32_t cpu)
{
	uint64_t next = timer_call_next_deadline(cpu);

	return state->stats.sched_deadline < next ? state->stats.sched_deadline : next;
}

static void
etimer_program(etimer_cpu_t *state, uint64_t deadline)
{
	if (deadline == TIMER_CALL_NO_DEADLINE) {
		g_etimer_hw->disarm();
	} else {
		g_etimer_hw->arm(deadline);
	}

	state->stats.armed_deadline = deadline;
	state->stats.programs++;
}

/* Interrupts masked, this CPU online. `exact`: program the earliest deadline even if that is later than now armed. */
static void
etimer_resync_cpu(etimer_cpu_t *state, uint32_t cpu, bool exact)
{
	uint64_t next = etimer_next_deadline(state, cpu);
	uint64_t armed = state->stats.armed_deadline;

	if (next == armed) return;

	if (next == TIMER_CALL_NO_DEADLINE || exact || next < armed) etimer_program(state, next);
}

void
etimer_cpu_start(void)
{
	if (!etimer_deadline_mode()) return;

	uint64_t irq_state = ml_irq_save();
	uint32_t cpu;
	etimer_cpu_t *state = etimer_local(&cpu);

	if (state != 0 && !state->online) {
		state->online = true;
		etimer_resync_cpu(state, cpu, true);
	}

	ml_irq_restore(irq_state);
}

bool
etimer_cpu_online(uint32_t cpu)
{
	return cpu < NXU_MAX_CPUS && etimer_deadline_mode() && g_etimer_cpus[cpu].online;
}

void
etimer_intr(void)
{
	uint32_t cpu;
	etimer_cpu_t *state = etimer_local(&cpu);

	if (state == 0 || !state->online) return;

	uint64_t now = clock_uptime_us();
	uint64_t armed = state->stats.armed_deadline;

	state->stats.interrupts++;

	/* The hardware is one-shot: whatever it was armed for has been delivered. */
	state->stats.armed_deadline = TIMER_CALL_NO_DEADLINE;

	if (armed != TIMER_CALL_NO_DEADLINE && now >= armed && now - armed > state->stats.max_latency_us)
		state->stats.max_latency_us = now - armed;

	bool did_work = timer_call_expire(cpu, now) != 0U;

	if (state->stats.sched_deadline <= now && g_etimer_sched_handler != 0) {
		state->stats.sched_ticks++;
		state->stats.sched_deadline = g_etimer_sched_handler(now);
		did_work = true;
	}

	if (!did_work) state->stats.idle_interrupts++;

	etimer_resync_cpu(state, cpu, false);
}

void
etimer_resync(void)
{
	if (!etimer_deadline_mode()) return;

	uint64_t irq_state = ml_irq_save();
	uint32_t cpu;
	etimer_cpu_t *state = etimer_local(&cpu);

	if (state != 0 && state->online) etimer_resync_cpu(state, cpu, false);

	ml_irq_restore(irq_state);
}

void
etimer_set_sched_handler(etimer_sched_handler_t handler)
{
	__atomic_store_n(&g_etimer_sched_handler, handler, __ATOMIC_RELEASE);
}

void
etimer_set_sched_deadline(uint64_t deadline)
{
	if (!etimer_deadline_mode()) return;

	uint64_t irq_state = ml_irq_save();
	uint32_t cpu;
	etimer_cpu_t *state = etimer_local(&cpu);

	if (state != 0 && state->stats.sched_deadline != deadline) {
		state->stats.sched_deadline = deadline;

		if (state->online) etimer_resync_cpu(state, cpu, true);
	}

	ml_irq_restore(irq_state);
}

void
etimer_stats(uint32_t cpu, etimer_stats_t *out)
{
	if (out == 0) return;

	if (cpu >= NXU_MAX_CPUS) {
		*out = (etimer_stats_t){0};
		return;
	}

	/* Another CPU's counters are read racily: they are diagnostics. */
	*out = g_etimer_cpus[cpu].stats;
}

/* Every online CPU's counters: in the verbose log, or always (a self-test failure explaining itself). */
static void
etimer_report(bool always)
{
	int (*print)(const char *, const char *, ...) = always ? kprintf_from : kverbosef_from;

	print(__func__, "event timer: %s\n", etimer_hw_name());

	for (uint32_t cpu = 0U; cpu < NXU_MAX_CPUS; cpu++) {
		if (!etimer_cpu_online(cpu)) continue;

		etimer_stats_t stats;
		timer_call_stats_t calls;

		etimer_stats(cpu, &stats);
		timer_call_stats(cpu, &calls);

		print(__func__,
			  "CPU %u: %llu interrupt(s), %llu with nothing due, %llu programming(s), %llu scheduler tick(s), "
			  "worst latency %llu us\n",
			  cpu, (unsigned long long)stats.interrupts, (unsigned long long)stats.idle_interrupts,
			  (unsigned long long)stats.programs, (unsigned long long)stats.sched_ticks,
			  (unsigned long long)stats.max_latency_us);
		print(__func__, "CPU %u: %llu timer call(s) entered, %llu cancelled, %llu run, %u pending, worst lateness %llu us\n",
			  cpu, (unsigned long long)calls.entered, (unsigned long long)calls.cancelled,
			  (unsigned long long)calls.expired, calls.pending, (unsigned long long)calls.max_lateness_us);
	}
}

void
etimer_dump(void)
{
	etimer_report(false);
}

/* ---- self-test ------------------------------------------------------------ */

#define ETIMER_TEST_RUNS 10U
#define ETIMER_TEST_DELAY_US 2000ULL

static volatile uint64_t g_etimer_test_fired_at;
static volatile uint32_t g_etimer_test_fired;

static void
etimer_test_fire(void *param0, void *param1)
{
	(void)param0;
	(void)param1;

	g_etimer_test_fired_at = clock_uptime_us();
	(void)__atomic_add_fetch(&g_etimer_test_fired, 1U, __ATOMIC_ACQ_REL);
}

#define ETIMER_CHECK(condition, what)                           \
	do {                                                        \
		if (!(condition)) {                                     \
			kprintf("FAIL %s\n", what);                         \
			return false;                                       \
		}                                                       \
	} while (0)

bool
etimer_self_test(bool strict)
{
	static struct timer_call call;

	/* Timed waits start with the event timer's first interrupt; give a periodic one a moment to come. */
	uint64_t waited_since = clock_uptime_us();

	while (!sched_is_ticking() && clock_uptime_us() - waited_since < 1000000ULL)
		(void)sched_yield();

	if (!sched_is_ticking()) {
		kprintf("FAIL timed sleeps did not become available within a second\n");
		return false;
	}

	/*
	 * How late is acceptable. Strict (a test run): a deadline timer is exact
	 * to the microsecond, but an emulated machine can be descheduled by its
	 * host for a while, and a periodic tick adds up to a tick (10 ms) of
	 * rounding. Otherwise (every boot's power-on self-test) lateness is
	 * reported, not judged, unless the timer is plainly broken: a loaded
	 * host must not fail a boot.
	 */
	uint64_t tolerance = !strict ? 500000ULL : etimer_deadline_mode() ? 5000ULL : 25000ULL;
	uint64_t sleep_past = strict ? tolerance : 20000ULL;
	uint64_t least = UINT64_MAX;
	uint64_t most = 0ULL;
	uint64_t total = 0ULL;

	timer_call_setup(&call, etimer_test_fire, 0, 0);

	for (uint32_t run = 0U; run < ETIMER_TEST_RUNS; run++) {
		__atomic_store_n(&g_etimer_test_fired, 0U, __ATOMIC_RELEASE);

		uint64_t deadline = clock_uptime_us() + ETIMER_TEST_DELAY_US;

		ETIMER_CHECK(!timer_call_enter(&call, deadline), "a new timer call was not pending");

		/* Sleep past it; the call wakes nobody, so this also checks it ran without help. */
		(void)sched_sleep_us(ETIMER_TEST_DELAY_US + sleep_past);

		uint64_t waited_since = clock_uptime_us();

		while (__atomic_load_n(&g_etimer_test_fired, __ATOMIC_ACQUIRE) == 0U &&
			   clock_uptime_us() - waited_since < tolerance)
			(void)sched_sleep_us(1000ULL);

		ETIMER_CHECK(__atomic_load_n(&g_etimer_test_fired, __ATOMIC_ACQUIRE) == 1U, "a timer call fires once");
		ETIMER_CHECK(g_etimer_test_fired_at >= deadline, "a timer call never fires before its deadline");

		uint64_t lateness = g_etimer_test_fired_at - deadline;

		if (lateness > tolerance) {
			kprintf("FAIL a timer call fired %llu us after its deadline (tolerance %llu us)\n",
					(unsigned long long)lateness, (unsigned long long)tolerance);
			etimer_report(true);
			return false;
		}

		if (lateness < least) least = lateness;
		if (lateness > most) most = lateness;
		total += lateness;
	}

	__atomic_store_n(&g_etimer_test_fired, 0U, __ATOMIC_RELEASE);
	(void)timer_call_enter(&call, clock_uptime_us() + 3000ULL);
	ETIMER_CHECK(timer_call_cancel_sync(&call), "a pending timer call can be cancelled");
	(void)sched_sleep_us(3000ULL + sleep_past);
	ETIMER_CHECK(__atomic_load_n(&g_etimer_test_fired, __ATOMIC_ACQUIRE) == 0U, "a cancelled timer call does not fire");
	ETIMER_CHECK(!timer_call_cancel_sync(&call), "a cancelled timer call is no longer pending");

	uint64_t overshoot = 0ULL;

	for (uint32_t run = 0U; run < ETIMER_TEST_RUNS / 2U; run++) {
		uint64_t start = clock_uptime_us();

		(void)sched_sleep_us(1000ULL);

		uint64_t elapsed = clock_uptime_us() - start;

		ETIMER_CHECK(elapsed >= 1000ULL, "a timed sleep lasts at least as long as asked");

		if (elapsed - 1000ULL > tolerance) {
			kprintf("FAIL a 1000 us sleep lasted %llu us (tolerance %llu us over)\n", (unsigned long long)elapsed,
					(unsigned long long)tolerance);
			etimer_report(true);
			return false;
		}

		if (elapsed - 1000ULL > overshoot) overshoot = elapsed - 1000ULL;
	}

	kprintf("timer calls fired %llu to %llu us after their deadlines (mean %llu us, %u runs), 1 ms sleeps overshot by "
			"at most %llu us (%s limit %llu us)\n",
			(unsigned long long)least, (unsigned long long)most, (unsigned long long)(total / ETIMER_TEST_RUNS),
			(uint32_t)ETIMER_TEST_RUNS, (unsigned long long)overshoot, strict ? "test" : "power-on",
			(unsigned long long)tolerance);
	return true;
}
