/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/rtclock.c
 *
 * Bringing up i386's timers, after XNU's rtclock: the monotonic clock rests
 * on the TSC (calibrated in i386_init, reported by clock_init), timer calls
 * are set up with the scheduler (sched_bootstrap), and this file decides at
 * the start of the "smp" phase what drives them from then on:
 *
 *   - each CPU's Local APIC timer, as a deadline event timer, when one of
 *     its modes works (kern/i386/lapic_timer.c). The PIT is stopped. Each
 *     secondary CPU starts its own timer as it comes up.
 *   - otherwise the PIT, as before: a periodic event timer at I386_TIMER_HZ
 *     on the boot CPU (kern/i386/irq.c), broadcast to the other CPUs.
 *
 * Then it connects the scheduler (sched_timer_init) and, once every CPU is
 * up, checks that each one's timer started.
 */

#include <kern/clock/etimer.h>
#include <kern/console/console.h>
#include <kern/i386/irq.h>
#include <kern/i386/lapic_timer.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>
#include <stdint.h>

void i386_rtclock_init(void);
bool i386_rtclock_init_complete(uint32_t online);

static bool g_rtclock_lapic;

void
i386_rtclock_init(void)
{
	g_rtclock_lapic = lapic_timer_init();

	if (!g_rtclock_lapic) {
		etimer_init(0);
		kprintf("the PIT drives every CPU at %u Hz (scheduler every %u interrupts)\n", (uint32_t)I386_TIMER_HZ,
				(uint32_t)(I386_TIMER_HZ / I386_SCHED_HZ));
	}

	sched_timer_init();
}

bool
i386_rtclock_init_complete(uint32_t online)
{
	bool complete = true;

	if (g_rtclock_lapic) {
		uint32_t ready = lapic_timer_cpus_ready();

		if (ready == online) {
			kprintf("all %u CPU timer(s) operational\n", ready);
		} else {
			kprintf("only %u of %u online CPU(s) started their timer\n", ready, online);
			complete = false;
		}
	}

	kprintf("timer subsystem initialization complete: %s, %u CPU(s)\n", etimer_hw_name(), online);
	return complete;
}
