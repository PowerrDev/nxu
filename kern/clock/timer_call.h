/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/clock/timer_call.h
 *
 * Timer calls: a function the kernel asks to have called at a deadline, after
 * XNU's timer_call. Each CPU keeps its own queue of them, sorted by deadline;
 * the event timer (kern/clock/etimer.h) programs that CPU's hardware timer
 * for the earliest one and, when it fires, runs every call that is due.
 *
 * A call is entered on the queue of the CPU that enters it. It runs there, in
 * interrupt context, with interrupts masked and no lock held, so a callback
 * must be short and must not sleep: waking a thread (the timed waits of
 * kern/sched_prism/waitq.c) is the typical one. Work that needs a thread
 * belongs on a thread the callback wakes.
 *
 * Deadlines are microseconds on the clock_uptime_us() clock (kern/clock/
 * clock.h). A call is never run before its deadline. How soon after it runs
 * depends on the event timer's mode: within microseconds with a deadline
 * timer, within the tick when the architecture only has a periodic one (then
 * every call is kept on the boot CPU's queue, which its tick runs).
 *
 * Concurrency. Entering, cancelling and expiry may race freely on any CPUs: a
 * call belongs to one queue at a time and moves between queues only through
 * a "migrating" state no other CPU acts on, so it is never on two queues or
 * run twice for one entry. timer_call_cancel_sync() also waits for a callback
 * already running on another CPU, after which the call's memory may be freed
 * or reused; plain timer_call_cancel() does not wait. A callback may enter
 * its own call again (a periodic call) but must not free it, and must not
 * cancel it synchronously (it would wait for itself).
 *
 * A zero-filled struct timer_call is a valid, never-entered call: embedding
 * one in a zeroed object needs no initialization beyond timer_call_setup().
 */

#ifndef NXU_KERN_CLOCK_TIMER_CALL_H
#define NXU_KERN_CLOCK_TIMER_CALL_H

#include <kern/lock.h>

#include <stdbool.h>
#include <stdint.h>

/* A deadline that never comes: "no timer call due". */
#define TIMER_CALL_NO_DEADLINE UINT64_MAX

typedef void (*timer_call_func_t)(void *param0, void *param1);

struct timer_call_queue;

struct timer_call {
	/* Queue links, sorted by deadline; protected by the owning queue's lock. */
	struct timer_call *next;
	struct timer_call *prev;

	uint64_t deadline;

	timer_call_func_t func;
	void *param0;
	void *param1;

	/*
	 * The queue this call belongs to: 0 before the first entry, a real queue
	 * after it, or the migrating marker while it moves between two queues.
	 * Read without a lock, then confirmed under that queue's lock.
	 */
	struct timer_call_queue *queue;

	/* On its queue's list, waiting for its deadline (the queue's lock). */
	bool pending;

	/* Callbacks of this call running right now (atomic; see cancel_sync). */
	uint32_t running;
};

typedef struct timer_call *timer_call_t;

/*
 * timer_call_init
 *
 * Set up every CPU's queue. Once, on the boot CPU, before the first entry.
 */
void timer_call_init(void);

/*
 * timer_call_setup
 *
 * Give `call` its function and parameters. Must not be called while the call
 * is pending or running (cancel it synchronously first); it does not touch
 * the call's queue state.
 */
void timer_call_setup(timer_call_t call, timer_call_func_t func, void *param0, void *param1);

/*
 * timer_call_enter
 *
 * Arrange for `call` to run at `deadline` on this CPU (on the boot CPU with a
 * periodic event timer), moving it from wherever it was pending. A deadline
 * already past runs at the next opportunity. Returns whether the call was
 * pending before (and so has been rescheduled rather than scheduled). Any
 * context, interrupts masked or not.
 */
bool timer_call_enter(timer_call_t call, uint64_t deadline);

/*
 * timer_call_cancel / timer_call_cancel_sync
 *
 * Take `call` off its queue if it is pending; returns whether it was. A
 * callback already running elsewhere is not stopped: timer_call_cancel_sync
 * waits for it to return as well, so the caller may free the call afterwards.
 * timer_call_cancel_sync spins (briefly: callbacks are short) and must not be
 * called from the call's own callback.
 */
bool timer_call_cancel(timer_call_t call);
bool timer_call_cancel_sync(timer_call_t call);

/* Whether `call` is waiting for its deadline right now (a hint: it can change at once). */
bool timer_call_is_pending(timer_call_t call);

/*
 * timer_call_next_deadline / timer_call_expire
 *
 * The event timer's half. timer_call_next_deadline is the earliest deadline
 * on CPU `cpu`'s queue (TIMER_CALL_NO_DEADLINE when it is empty).
 * timer_call_expire runs, in deadline order, every call on that queue whose
 * deadline is at or before `now`, on the calling CPU, which must be `cpu`
 * (the queue's own CPU, or the boot CPU for the shared queue of a periodic
 * event timer). Returns how many ran. Interrupt context.
 */
uint64_t timer_call_next_deadline(uint32_t cpu);
uint32_t timer_call_expire(uint32_t cpu, uint64_t now);

/*
 * timer_call_queue_migrate
 *
 * Move every pending call from CPU `from`'s queue to CPU `to`'s, keeping
 * their deadlines: for a CPU that stops taking timer interrupts. Must be
 * called on CPU `to`, which then reprograms its own timer for the moved
 * calls. Returns how many moved.
 */
uint32_t timer_call_queue_migrate(uint32_t from, uint32_t to);

/*
 * Per-queue counters, for diagnostics: calls entered, cancelled while
 * pending, run, how many are pending now, and the worst lateness seen (how
 * long after its deadline a call ran, in microseconds).
 */
typedef struct {
	uint64_t entered;
	uint64_t cancelled;
	uint64_t expired;
	uint32_t pending;
	uint64_t max_lateness_us;
} timer_call_stats_t;

void timer_call_stats(uint32_t cpu, timer_call_stats_t *out);

/*
 * timer_call_self_test
 *
 * Order, reschedule, cancel, migrate and run calls on private queues with a
 * synthetic clock. Touches no CPU's real queue. Returns false on the first
 * failure (and says which).
 */
bool timer_call_self_test(void);

#endif
