/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/clock/etimer.h
 *
 * The event timer, after XNU's etimer: decides, for each CPU, when its next
 * timer interrupt has to happen, and has the architecture's hardware timer
 * deliver it then and not before. Two sources of deadlines feed it:
 *
 *   - the CPU's timer call queue (kern/clock/timer_call.h): timed waits and
 *     any other kernel callback due at a time;
 *   - the scheduler, which asks for its own tick while the CPU runs a thread
 *     (quantum accounting, preemption) and for none while the CPU is idle
 *     (kern/sched_prism/sched_timer.c).
 *
 * The earliest of the two is what the hardware is programmed for. When it
 * fires, etimer_intr() runs the calls that are due, gives the scheduler its
 * tick if that is due, and programs the next deadline. A CPU with nothing due
 * takes no timer interrupts at all.
 *
 * The hardware is behind etimer_hw_t: the event timer knows nothing about the
 * Local APIC (kern/i386/lapic_timer.c) or any other device, and the hardware
 * driver knows nothing about what the deadlines are for.
 *
 * Modes. With a hardware deadline timer registered (etimer_init with an ops
 * table), every CPU has its own queue and deadlines. Without one (an
 * architecture that only has a periodic tick, or i386 when the Local APIC
 * timer cannot be used) the event timer is in periodic mode: every timer call
 * goes on the boot CPU's queue and that CPU's tick runs it, at the tick's
 * resolution; the scheduler keeps its periodic tick on every CPU.
 */

#ifndef NXU_KERN_CLOCK_ETIMER_H
#define NXU_KERN_CLOCK_ETIMER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The hardware timer, one instance per CPU, each operated only by its own
 * CPU with interrupts masked.
 *
 *   arm(deadline)  interrupt this CPU once the clock reaches `deadline`
 *                  (microseconds, clock_uptime_us), replacing any earlier
 *                  programming. A deadline already past interrupts at once.
 *                  A device whose range is shorter than the distance may
 *                  interrupt sooner; the event timer reprograms it then.
 *   disarm()       no interrupt until the next arm.
 *
 * `name` is how the boot log and diagnostics refer to it.
 */
typedef struct {
	const char *name;
	void (*arm)(uint64_t deadline_us);
	void (*disarm)(void);
} etimer_hw_t;

/*
 * etimer_init
 *
 * Choose the mode: `hw` is the deadline timer every CPU will use, or 0 for
 * periodic mode. Once, on the boot CPU, after timer_call_init and before any
 * CPU calls etimer_cpu_start. No CPU takes event timer interrupts until it
 * has called etimer_cpu_start.
 */
void etimer_init(const etimer_hw_t *hw);

/* Whether a deadline timer is driving the CPUs (false in periodic mode, and before etimer_init). */
bool etimer_deadline_mode(void);

/*
 * etimer_cpu_start
 *
 * This CPU's hardware timer is set up and its interrupt routed to
 * etimer_intr: start programming it. On every CPU, with interrupts masked or
 * not. Does nothing in periodic mode.
 */
void etimer_cpu_start(void);

/* Whether this CPU has started (etimer_cpu_start) in deadline mode. */
bool etimer_cpu_online(uint32_t cpu);

/*
 * etimer_intr
 *
 * The hardware timer interrupted this CPU. Interrupt context, interrupts
 * masked; the architecture has already acknowledged the interrupt at its
 * controller.
 */
void etimer_intr(void);

/*
 * etimer_resync
 *
 * Something this CPU's next deadline depends on changed (a timer call became
 * the earliest, the scheduler's deadline moved): program the hardware for
 * the earliest deadline now. Cheap when nothing changed. Any context.
 */
void etimer_resync(void);

/*
 * etimer_set_sched_handler / etimer_set_sched_deadline
 *
 * The scheduler's connection. The handler runs, on the CPU concerned, from
 * etimer_intr once that CPU's scheduler deadline has passed, and returns the
 * next one (TIMER_CALL_NO_DEADLINE for none). etimer_set_sched_deadline sets
 * this CPU's scheduler deadline outside the handler (a CPU going idle or
 * busy) and reprograms the timer. Interrupts masked.
 */
typedef uint64_t (*etimer_sched_handler_t)(uint64_t now);

void etimer_set_sched_handler(etimer_sched_handler_t handler);
void etimer_set_sched_deadline(uint64_t deadline);

/*
 * Per-CPU counters, for diagnostics: interrupts taken, how many found nothing
 * due (a cancelled deadline, a device that cannot reach the whole distance),
 * hardware programmings, scheduler ticks given, and the worst interrupt
 * latency seen (how long after the programmed deadline the interrupt ran).
 */
typedef struct {
	uint64_t interrupts;
	uint64_t idle_interrupts;
	uint64_t programs;
	uint64_t sched_ticks;
	uint64_t max_latency_us;
	uint64_t armed_deadline;
	uint64_t sched_deadline;
} etimer_stats_t;

void etimer_stats(uint32_t cpu, etimer_stats_t *out);

/* Print every online CPU's counters (verbose log). */
void etimer_dump(void);

/* The hardware timer's name, or "periodic tick". */
const char *etimer_hw_name(void);

/*
 * etimer_self_test
 *
 * On the live system, with the calling thread able to sleep: timer calls
 * fire once and never early, cancelled ones do not fire, and timed sleeps
 * last at least as long as asked. Logs the latencies it measured. `strict`
 * (a test run) also fails on latency beyond a few milliseconds; otherwise
 * (a power-on self-test, on every boot) only a timer half a second late
 * fails, so a loaded host cannot fail a boot. Returns false on the first
 * failure (and says which).
 */
bool etimer_self_test(bool strict);

#endif
