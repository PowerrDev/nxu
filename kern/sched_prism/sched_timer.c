/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/sched_prism/sched_timer.c
 *
 * The scheduler's deadlines on a deadline event timer (kern/clock/etimer.h).
 *
 * The scheduler's policy is written in ticks of SCHED_TICK_US: quanta and
 * MLFQ allotments are charged a tick at a time to the running thread, a CPU's
 * queue is boosted every SCHED_MLFQ_BOOST_INTERVAL_TICKS, starvation and
 * balancing are checked every few ticks. None of that needs a tick while a
 * CPU is idle: nothing runs to be charged, its run queue is empty, and the
 * clock waiting times are measured on (run_queue_clock) is derived from the
 * monotonic clock. So a CPU asks the event timer for a tick every
 * SCHED_TICK_US only while it runs a thread, and for none while it idles:
 *
 *   busy -> idle   the tick deadline is withdrawn (an idle CPU sleeps until
 *                  a timer call or a device or another CPU wakes it);
 *   idle -> busy   the first tick is due SCHED_TICK_US after the CPU got
 *                  busy, and the ticks it skipped while idle are counted
 *                  towards the boost interval and the CPU's tick count, as
 *                  if it had taken them.
 *
 * The transitions are seen in sched_switch (sched_timer_dispatch). An idle
 * CPU also misses the balancing its tick used to do; a busy CPU with waiting
 * threads an idle CPU could take sends it a reschedule IPI instead (see
 * sched_tick), and the idle CPU balances on the way back from it.
 *
 * With a periodic event timer none of this runs: the architecture calls
 * sched_tick() from its tick on every CPU, as it always has.
 */

#include <kern/sched_prism/sched.h>

#include <kern/clock/clock.h>
#include <kern/clock/etimer.h>
#include <kern/clock/timer_call.h>
#include <kern/console/console.h>
#include <kern/machine/machine_routines.h>

#include <stdbool.h>
#include <stdint.h>

static bool g_sched_timer_enabled;

/* This CPU's scheduler deadline passed (etimer_intr, interrupt context): give it its tick, and say when the next is. */
static uint64_t
sched_timer_expire(uint64_t now)
{
	processor_t processor = current_processor();

	if (processor == 0 || processor->sched_tick_deadline_us == 0ULL) return TIMER_CALL_NO_DEADLINE;

	sched_tick();

	/* Keep the phase; after a stall (the machine was descheduled) carry on from now rather than catch up. */
	uint64_t next = processor->sched_tick_deadline_us + SCHED_TICK_US;

	if (next <= now) next = now + SCHED_TICK_US;

	processor->sched_tick_deadline_us = next;
	return next;
}

bool
sched_timer_enabled(void)
{
	return __atomic_load_n(&g_sched_timer_enabled, __ATOMIC_ACQUIRE);
}

void
sched_timer_dispatch(processor_t processor, bool idle_next, uint64_t now)
{
	if (!sched_timer_enabled() || processor == 0) return;

	if (idle_next) {
		if (processor->sched_tick_deadline_us == 0ULL) return;

		processor->sched_tick_deadline_us = 0ULL;
		processor->sched_idle_since_us = now;
		etimer_set_sched_deadline(TIMER_CALL_NO_DEADLINE);
		return;
	}

	if (processor->sched_tick_deadline_us != 0ULL) return;

	/* The ticks this CPU would have taken while idle: they move the boost along, and count as idle ticks. */
	if (processor->sched_idle_since_us != 0ULL && now > processor->sched_idle_since_us) {
		uint64_t skipped = (now - processor->sched_idle_since_us) / SCHED_TICK_US;
		uint64_t boost = (uint64_t)processor->boost_ticks + skipped;

		processor->boost_ticks =
			boost > SCHED_MLFQ_BOOST_INTERVAL_TICKS ? SCHED_MLFQ_BOOST_INTERVAL_TICKS : (uint32_t)boost;
		processor->sched_ticks += skipped;
	}

	processor->sched_idle_since_us = 0ULL;
	processor->sched_tick_deadline_us = now + SCHED_TICK_US;
	etimer_set_sched_deadline(processor->sched_tick_deadline_us);
}

void
sched_timer_init(void)
{
	kprintf("connecting scheduler deadlines\n");

	if (!etimer_deadline_mode()) {
		/* The tick still drives the scheduler; it is what makes timed waits work from now on as well. */
		kprintf("periodic event timer: the scheduler keeps its %llu Hz tick on every CPU\n",
				(unsigned long long)(1000000ULL / SCHED_TICK_US));
		return;
	}

	etimer_set_sched_handler(sched_timer_expire);

	uint64_t irq_state = ml_irq_save();
	processor_t processor = current_processor();
	bool idle = processor->active_thread == 0 || thread_is_idle(processor->active_thread);

	__atomic_store_n(&g_sched_timer_enabled, true, __ATOMIC_RELEASE);

	/* The boot CPU is running the boot thread: its tick starts now. */
	processor->sched_tick_deadline_us = 0ULL;
	processor->sched_idle_since_us = 0ULL;
	sched_timer_dispatch(processor, idle, clock_uptime_us());

	ml_irq_restore(irq_state);

	/* Timed waits end on timer calls from here on. */
	sched_set_ticking();

	kprintf("per-CPU quantum timers enabled: a %llu Hz tick while a CPU runs a thread, none while it idles\n",
			(unsigned long long)(1000000ULL / SCHED_TICK_US));
}
