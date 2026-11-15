/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/sched_prism/sched.c
 *
 * Implements the per-CPU multilevel feedback queue scheduler.
 */
/*
 *
 * The MLFQ scheduler, one run queue per CPU.
 *
 * SMP locking model
 * -----------------
 *
 * There is no scheduler-wide lock. Three kinds of lock exist, all spinlocks
 * that are taken with interrupts masked:
 *
 *   processor.runq_lock    One per CPU. Protects that CPU's run queue: which
 *                          threads are queued, their thread->runq and
 *                          run-queue links, and the CPU's active_thread and
 *                          next_thread. It also serialises the MLFQ policy on
 *                          the queued threads (levels, quanta, boost) and the
 *                          active thread's own MLFQ fields.
 *
 *   thread.sched_lock      One per thread. Protects thread->on_cpu and
 *                          thread->wakeup_deferred, the hand-off between the
 *                          CPU switching a thread out and whoever wakes it.
 *
 *   g_thread_lock          (kern/process/thread.c) Thread state bits, task
 *                          membership, affinity. Innermost: nothing is taken
 *                          while it is held.
 *
 * Order, outermost first:  runq_lock(cpu a) -> runq_lock(cpu b > a) ->
 * g_thread_lock. sched_lock is a leaf: it may be taken under a runq_lock
 * (sched_switch marks its incoming thread), never the other way round. Code that needs two run queues (migration,
 * balancing) takes them in ascending logical CPU id, and no path holds a runq_lock while it waits for anything else. A
 * CPU takes only its own runq_lock in the hot path (sched_switch, sched_tick); another CPU's is taken only to place or
 * migrate a thread onto it.
 *
 * Invariants (checked by sched_validate):
 *   - A thread is on at most one run queue, and only while it is neither
 *     running nor waiting.
 *   - thread->on_cpu is set exactly from the moment a CPU chooses a thread
 *     until that CPU has switched off the thread's stack. A thread with on_cpu
 *     set is never queued: a wakeup that arrives in that window is recorded in
 *     wakeup_deferred and carried out by the CPU that is switching away
 *     (sched_finish_switch), after the thread's context has been saved. This
 *     is what stops two CPUs from ever running one thread.
 *   - A running thread is never migrated: only RUNNABLE, queued threads move,
 *     and they move under both run queues' locks.
 *   - A CPU's idle thread is never queued, never migrates, and is the only
 *     thing its CPU runs when nothing else is queued.
 *
 * Thread states (thread->state bits, see kern/process/thread.h):
 *   NEW       TH_SUSP, not started      -> sched_thread_start
 *   RUNNABLE  TH_RUN, on a run queue    -> chosen by a CPU
 *   RUNNING   TH_RUN, on_cpu != 0       -> yield/preempt: RUNNABLE again;
 *                                          block: WAITING; terminate: DEAD
 *   WAITING   TH_WAIT (on a wait queue) -> woken: RUNNABLE
 *   DEAD      TH_TERMINATE              -> reaped by the CPU that left it
 * TH_SUSP (hold) can combine with any of these and keeps a thread from being
 * queued until released.
 */

#include <kern/boot/boot_args.h>
#include <kern/console/console.h>
#include <kern/sched_prism/sched.h>

#include <kern/clock/clock.h>
#include <kern/clock/etimer.h>
#include <kern/clock/timer_call.h>
#include <kern/sched_prism/waitq.h>
#include <kern/ipi.h>
#include <kern/machine/cpu.h>
#include <kern/machine/machine_routines.h>
#include <kern/machine/smp.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/syscall/syscall_defs.h>
#include <platform/uart.h>
#include <vm/address_space.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	thread_t bootstrap_thread;
	task_t kernel_task;
	bool initialized;
} sched_state_t;

typedef enum { SCHED_SWITCH_YIELD, SCHED_SWITCH_BLOCK, SCHED_SWITCH_EXIT, SCHED_SWITCH_PREEMPT } sched_switch_reason_t;

static sched_state_t g_sched;

/* Every this many ticks an idle or underloaded CPU looks for work to pull from a busier one. */
#define SCHED_BALANCE_INTERVAL_TICKS 4U

/*
 * Statistics for the SMP tests: how many CPUs are executing threads of user
 * processes at this moment, and the largest that number has been since it was
 * last reset. Updated in sched_switch, under the switching CPU's run queue lock,
 * with atomics (they are shared by all CPUs).
 */
static uint32_t g_sched_user_running;
static uint32_t g_sched_user_peak;

static bool
sched_thread_is_user(thread_t thread)
{
	return thread != 0 && (thread->flags & TH_FLAG_KERNEL) == 0U && !thread_is_idle(thread);
}

uint32_t
sched_user_running_peak(void)
{
	return __atomic_load_n(&g_sched_user_peak, __ATOMIC_ACQUIRE);
}

void
sched_user_running_reset_peak(void)
{
	__atomic_store_n(&g_sched_user_peak, __atomic_load_n(&g_sched_user_running, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

/*
 * Timed sleep. Sleepers wait on one queue, each with its own deadline
 * (waitq_block_deadline): a timer call that ends the sleep when it is due,
 * to the microsecond with a deadline event timer, to the tick with a
 * periodic one. Nothing wakes the queue as a whole; a sleeper woken early
 * (an interrupted wait) re-checks its deadline and sleeps again.
 *
 * Until the first tick nothing would wake them: the kernel's process tests
 * (boot_test_storage) run user processes before kern_start_scheduler starts
 * the timer. Sleepers yield until their deadline instead, as they would
 * have spun before there was a sleep at all.
 */
static waitq_t g_sched_sleep_queue;
static uint32_t g_sched_sleepers;
static bool g_sched_ticking;

static bool
sched_sleep_yield_until(uint64_t deadline)
{
	while (timer_get_microseconds() < deadline) {
		if (!sched_yield()) return false;
	}
	return true;
}

/* Kickable sleepers (sched_sleep_until_kicked) wait here; a kick bumps g_sched_kicks. */
static waitq_t g_sched_kick_queue;
static uint32_t g_sched_kick_sleepers;
static uint32_t g_sched_kicks;

bool
sched_sleep_us(uint64_t microseconds)
{
	if (!g_sched.initialized || current_thread() == 0 || thread_is_idle(current_thread())) return false;

	uint64_t deadline = timer_get_microseconds() + microseconds;

	if (!__atomic_load_n(&g_sched_ticking, __ATOMIC_ACQUIRE)) return sched_sleep_yield_until(deadline);

	(void)__atomic_add_fetch(&g_sched_sleepers, 1U, __ATOMIC_ACQ_REL);

	while (timer_get_microseconds() < deadline) {
		uint32_t seq = waitq_seq(&g_sched_sleep_queue);

		if (timer_get_microseconds() >= deadline) break;

		if (waitq_block_deadline(&g_sched_sleep_queue, seq, false, deadline) == WAITQ_INTERRUPTED) break;
	}

	(void)__atomic_sub_fetch(&g_sched_sleepers, 1U, __ATOMIC_ACQ_REL);
	return true;
}

/*
 * A queue nobody wakes: the deadline ends the sleep, or a signal does
 * (waitq_interrupt unlinks the thread). It lives on this thread's stack for
 * exactly the length of the wait, which is all it is ever reached by.
 */
bool
sched_sleep_us_interruptible(uint64_t microseconds, bool *interrupted)
{
	*interrupted = false;

	if (!g_sched.initialized || current_thread() == 0 || thread_is_idle(current_thread())) return false;

	uint64_t deadline = timer_get_microseconds() + microseconds;
	waitq_t alone;

	waitq_init(&alone);

	while (timer_get_microseconds() < deadline) {
		if (waitq_block_deadline(&alone, waitq_seq(&alone), true, deadline) == WAITQ_INTERRUPTED) {
			*interrupted = true;
			break;
		}
	}

	return true;
}

void
sched_deadline_tick(void)
{
	if (!g_sched.initialized || etimer_deadline_mode()) return;

	/* This interrupt runs timer calls, so timed waits work from the first one, not only from the first tick. */
	__atomic_store_n(&g_sched_ticking, true, __ATOMIC_RELEASE);
	(void)timer_call_expire(0U, clock_uptime_us());
}

void
sched_set_ticking(void)
{
	__atomic_store_n(&g_sched_ticking, true, __ATOMIC_RELEASE);
}

bool
sched_is_ticking(void)
{
	return __atomic_load_n(&g_sched_ticking, __ATOMIC_ACQUIRE);
}

uint32_t
sched_kick_sequence(void)
{
	return __atomic_load_n(&g_sched_kicks, __ATOMIC_ACQUIRE);
}

bool
sched_sleep_until_kicked(uint64_t microseconds)
{
	return sched_sleep_until_kicked_seq(microseconds, sched_kick_sequence(), false);
}

bool
sched_sleep_until_kicked_seq(uint64_t microseconds, uint32_t kicks, bool interruptible)
{
	if (!g_sched.initialized || current_thread() == 0 || thread_is_idle(current_thread())) return false;

	uint64_t now = timer_get_microseconds();
	uint64_t deadline = microseconds > UINT64_MAX - now ? UINT64_MAX : now + microseconds;
	bool completed = true;

	if (!__atomic_load_n(&g_sched_ticking, __ATOMIC_ACQUIRE)) return sched_sleep_yield_until(deadline);

	(void)__atomic_add_fetch(&g_sched_kick_sleepers, 1U, __ATOMIC_ACQ_REL);

	while (timer_get_microseconds() < deadline) {
		uint32_t seq = waitq_seq(&g_sched_kick_queue);

		if (__atomic_load_n(&g_sched_kicks, __ATOMIC_ACQUIRE) != kicks || timer_get_microseconds() >= deadline) break;

		if (waitq_block_deadline(&g_sched_kick_queue, seq, interruptible, deadline) == WAITQ_INTERRUPTED) {
			completed = false;
			break;
		}

	}

	(void)__atomic_sub_fetch(&g_sched_kick_sleepers, 1U, __ATOMIC_ACQ_REL);
	return completed;
}

void
sched_kick_sleepers(void)
{
	(void)__atomic_add_fetch(&g_sched_kicks, 1U, __ATOMIC_ACQ_REL);
	if (__atomic_load_n(&g_sched_kick_sleepers, __ATOMIC_ACQUIRE) != 0U) waitq_wake_all(&g_sched_kick_queue);
}

static bool sched_switch(sched_switch_reason_t reason);
static processor_t sched_select_cpu(thread_t thread);
static bool sched_enqueue_on(processor_t target, thread_t thread, run_queue_placement_t placement);
static bool sched_qos_self_test(void);

/*
 * sched_fatal
 */
static __attribute__((noreturn)) void
sched_fatal(const char *message)
{
	ml_irq_disable();
	kputln(message);

	for (;;) {
		cpu_wait_for_event();
	}
}

/*
 * MLFQ level tables.
 *
 * Each level is scheduled at one fixed run-queue priority, so the existing
 * fixed-priority run queue already gives MLFQ its "always run the highest
 * non-empty line" (Rule 1) and "share a line round-robin" (Rule 2)
 * behavior. Only the level<->priority/quantum mapping is policy here.
 */
static const uint32_t sched_mlfq_quantum[SCHED_MLFQ_LEVELS] = {4U, 8U, 16U, 32U};

/*
 * Quality-of-service bands (NXU_QOS_*, after XNU's QoS classes).
 *
 * A thread's MLFQ level picks a priority inside its QoS band, and the bands
 * never overlap: a CPU-bound thread sinks to the bottom of its own band and
 * no further, so the compositor and the desktop -- which render every frame
 * and so do use up their quanta -- can never fall below a newly started app
 * or a daemon walking the disk. Starvation is the price of strict bands;
 * sched_starvation_scan pays it.
 */
static const uint16_t sched_qos_band[NXU_QOS_COUNT][SCHED_MLFQ_LEVELS] = {
	[NXU_QOS_USER_INTERACTIVE] = {63U, 61U, 59U, 57U},
	[NXU_QOS_USER_INITIATED] = {54U, 52U, 50U, 48U},
	[NXU_QOS_DEFAULT] = {45U, 41U, 37U, 33U},
	[NXU_QOS_UTILITY] = {30U, 27U, 24U, 21U},
	[NXU_QOS_BACKGROUND] = {16U, 12U, 8U, 4U},
};

_Static_assert(sizeof(sched_qos_band[0]) / sizeof(sched_qos_band[0][0]) == SCHED_MLFQ_LEVELS,
			   "every QoS band must have exactly SCHED_MLFQ_LEVELS entries");

static const char *const sched_qos_names[NXU_QOS_COUNT] = {
	[NXU_QOS_USER_INTERACTIVE] = "user-interactive",
	[NXU_QOS_USER_INITIATED] = "user-initiated",
	[NXU_QOS_DEFAULT] = "default",
	[NXU_QOS_UTILITY] = "utility",
	[NXU_QOS_BACKGROUND] = "background",
};

/* What a yielding thread is depressed to (see thread->yield_depressed): the
 * bottom of the DEFAULT band. */
#define SCHED_YIELD_DEPRESS_PRIORITY 33U
/* Consecutive yields without blocking that make a thread a spinner. */
#define SCHED_YIELD_DEPRESS_STREAK 16U

/*
 * Starvation lifts, in two tiers. A thread kept waiting SCHED_STARVATION_TICKS
 * is lifted just under the USER_INTERACTIVE band: above every other band, so
 * it runs, but never level with the compositor and the desktop -- lifting
 * starved work to the very top had two CPU hogs taking turns at the user
 * interface's priority. Only one kept waiting SCHED_STARVATION_EMERGENCY_TICKS
 * (which takes an interactive thread that never blocks or yields) goes to the
 * top.
 */
#define SCHED_STARVATION_PRIORITY 56U
#define SCHED_STARVATION_TICKS 50U
#define SCHED_STARVATION_EMERGENCY_PRIORITY THREAD_PRIORITY_MAX
#define SCHED_STARVATION_EMERGENCY_TICKS 200U
/* How often each CPU looks for starved threads, and how many it lifts. */
#define SCHED_STARVATION_SCAN_TICKS 10U
#define SCHED_STARVATION_LIFT_MAX 4U

/*
 * The QoS a thread is scheduled with: its task's. Kernel threads are system
 * work the user interface waits on (I/O completion, input), so they share
 * the top band; the MLFQ still sinks a busy one to the bottom of it.
 */
/* QoS classes in order, lowest first; and each class's place in it. */
static const uint8_t sched_qos_by_rank[NXU_QOS_COUNT] = {NXU_QOS_BACKGROUND, NXU_QOS_UTILITY, NXU_QOS_DEFAULT,
														 NXU_QOS_USER_INITIATED, NXU_QOS_USER_INTERACTIVE};

static uint8_t
sched_qos_rank(uint8_t qos)
{
	for (uint8_t rank = 0U; rank < NXU_QOS_COUNT; rank++)
		if (sched_qos_by_rank[rank] == qos) return rank;
	return sched_qos_rank(NXU_QOS_DEFAULT);
}

/* How long a donation lasts unless renewed: long enough to serve a request,
 * short enough that one leaked into a reply's receiver does not matter. */
#define SCHED_QOS_DONATION_TICKS 20U

/* sched.qos=0 on the boot command line: every thread in the DEFAULT band,
 * the plain MLFQ, for comparing with and without QoS. */
static bool g_sched_qos_enabled = true;

uint8_t
sched_task_effective_qos(task_t task)
{
	if (task == 0 || !g_sched_qos_enabled) return NXU_QOS_DEFAULT;
	if ((task->flags & TASK_FLAG_KERNEL) != 0U) return NXU_QOS_USER_INTERACTIVE;

	uint8_t own = task->qos < NXU_QOS_COUNT ? task->qos : NXU_QOS_DEFAULT;
	uint8_t donated = __atomic_load_n(&task->qos_override, __ATOMIC_RELAXED);
	uint32_t until = __atomic_load_n(&task->qos_override_until, __ATOMIC_RELAXED);

	if (donated == 0U || donated > NXU_QOS_COUNT || (int32_t)(until - run_queue_clock()) <= 0) return own;
	return donated - 1U > sched_qos_rank(own) ? sched_qos_by_rank[donated - 1U] : own;
}

void
sched_task_qos_donate(task_t task, uint8_t qos)
{
	if (task == 0 || qos >= NXU_QOS_COUNT) return;

	uint8_t rank = sched_qos_rank(qos);

	if (rank <= sched_qos_rank(task->qos)) return;

	uint8_t current = __atomic_load_n(&task->qos_override, __ATOMIC_RELAXED);
	bool live = current != 0U && (int32_t)(task->qos_override_until - run_queue_clock()) > 0;

	if (!live || current - 1U < rank) __atomic_store_n(&task->qos_override, (uint8_t)(rank + 1U), __ATOMIC_RELAXED);
	__atomic_store_n(&task->qos_override_until, run_queue_clock() + SCHED_QOS_DONATION_TICKS, __ATOMIC_RELAXED);
}

void
sched_task_qos_handling(task_t task, uint8_t qos)
{
	if (task == 0) return;

	if (qos >= NXU_QOS_COUNT || sched_qos_rank(qos) <= sched_qos_rank(task->qos)) {
		__atomic_store_n(&task->qos_override, (uint8_t)0U, __ATOMIC_RELAXED);
		return;
	}
	__atomic_store_n(&task->qos_override, (uint8_t)(sched_qos_rank(qos) + 1U), __ATOMIC_RELAXED);
	__atomic_store_n(&task->qos_override_until, run_queue_clock() + SCHED_QOS_DONATION_TICKS, __ATOMIC_RELAXED);
}

static uint8_t
sched_thread_qos(const struct thread *thread)
{
	return sched_task_effective_qos(thread != 0 ? thread->task : 0);
}

/* The run-queue priority of an MLFQ level within a QoS band. */
static uint16_t
sched_qos_priority(uint8_t qos, uint8_t level)
{
	if (qos >= NXU_QOS_COUNT) qos = NXU_QOS_DEFAULT;
	if (level > SCHED_MLFQ_BOTTOM_LEVEL) level = SCHED_MLFQ_BOTTOM_LEVEL;
	return sched_qos_band[qos][level];
}

/* Whether `priority` is the top of some band: only level-0 threads (and
 * lifted ones) are queued there. */
static bool
sched_priority_is_band_top(uint16_t priority)
{
	for (uint8_t qos = 0U; qos < NXU_QOS_COUNT; qos++)
		if (sched_qos_band[qos][SCHED_MLFQ_TOP_LEVEL] == priority) return true;
	return false;
}

const char *
sched_qos_name(uint8_t qos)
{
	return qos < NXU_QOS_COUNT ? sched_qos_names[qos] : "unknown";
}


_Static_assert(sizeof(sched_mlfq_quantum) / sizeof(sched_mlfq_quantum[0]) == SCHED_MLFQ_LEVELS,
			   "sched_mlfq_quantum must have exactly SCHED_MLFQ_LEVELS entries");

uint16_t
sched_mlfq_level_priority(uint8_t level)
{
	return sched_qos_priority(NXU_QOS_DEFAULT, level);
}

uint16_t
sched_thread_level_priority(thread_t thread, uint8_t level)
{
	return sched_qos_priority(sched_thread_qos(thread), level);
}

uint32_t
sched_mlfq_level_quantum(uint8_t level)
{
	if (level > SCHED_MLFQ_BOTTOM_LEVEL) level = SCHED_MLFQ_BOTTOM_LEVEL;
	return sched_mlfq_quantum[level];
}

/*
 * sched_mlfq_assign
 *
 * Move a thread to a given MLFQ level: update its feedback level, the
 * run-queue priority that level is scheduled at and the quantum it is
 * granted there. The idle thread never participates in MLFQ.
 *
 * Callers are responsible for keeping run-queue membership consistent -
 * this only touches the thread's own fields, so it must be called either
 * before the thread is enqueued or while it is off any run queue.
 */
static void
sched_mlfq_assign(thread_t thread, uint8_t level)
{
	if (thread == 0 || thread_is_idle(thread)) return;

	if (level > SCHED_MLFQ_BOTTOM_LEVEL) level = SCHED_MLFQ_BOTTOM_LEVEL;

	uint16_t priority = sched_qos_priority(sched_thread_qos(thread), level);

	thread->mlfq_level = level;
	thread->sched_pri = priority;
	thread->base_pri = priority;
	thread->qos_promoted = false;
	thread->yield_depressed = false;
	thread->quantum_remaining = sched_mlfq_quantum[level];
	thread->mlfq_ticks = 0U;
}

/*
 * sched_mlfq_charge_tick
 *
 * Charge one timer tick to the thread that was running, and demote it when
 * it has spent what its level allows. Returns whether it was demoted; the
 * caller then has to ask for preemption, because a thread that just lost
 * priority may no longer be the best one to run.
 *
 * Two things can spend a level. The quantum runs out when the thread runs
 * that long in one go (Rule 4 as before). The allotment, one quantum's worth
 * of ticks at the level, runs out however the ticks were spread: every
 * dispatch refills the quantum, so a thread that keeps yielding never
 * exhausts it and, without the allotment, would stay at level 0 for ever and
 * starve everything below it (the boot chime thread, bootd) until the boost.
 * Blocking is what shows a thread is not CPU-bound, so that is what clears
 * the allotment (see sched_switch).
 *
 * The bottom level has nowhere to fall to; running out of quantum there only
 * grants a fresh one. Called with the CPU's runq_lock held.
 */
static bool
sched_mlfq_charge_tick(thread_t thread, bool *quantum_expired)
{
	if (thread->quantum_remaining != 0U) thread->quantum_remaining--;

	if (thread->mlfq_ticks != UINT32_MAX) thread->mlfq_ticks++;

	bool expired = thread->quantum_remaining == 0U;
	bool spent =
		thread->mlfq_level < SCHED_MLFQ_BOTTOM_LEVEL && thread->mlfq_ticks >= sched_mlfq_quantum[thread->mlfq_level];

	if (quantum_expired != 0) *quantum_expired = expired;

	if (!expired && !spent) return false;

	/*
	 * sched_mlfq_assign() also grants the new level's quantum and clears the
	 * allotment, so there is no separate reset here.
	 */
	uint8_t level = thread->mlfq_level;

	if (level < SCHED_MLFQ_BOTTOM_LEVEL) level++;
	sched_mlfq_assign(thread, level);
	return true;
}

/*
 * sched_mlfq_blocked
 *
 * A thread that sleeps is not CPU-bound: what it ran at its level so far
 * no longer counts against it. Called with the CPU's runq_lock held.
 */
static void
sched_mlfq_blocked(thread_t thread)
{
	thread->mlfq_ticks = 0U;
	/* Waiting for something real: the yield depression is over. */
	thread->yield_depressed = false;
	thread->yield_streak = 0U;
}

/*
 * sched_mlfq_boost_locked
 *
 * The "equalizer" rule: drain every level below the top queue and put its
 * threads back at level 0 with a fresh top-level quantum, then do the same
 * for the currently running thread. Called with the CPU's runq_lock held.
 */
static void
sched_mlfq_boost_locked(processor_t processor)
{
	if (processor == 0) return;

	/*
	 * Highest priority first: a boosted thread moves up to the top of its own
	 * band, which is above where it was and so already past, never seen twice.
	 * The band tops hold only level-0 threads and are left alone.
	 */
	for (int32_t priority = THREAD_PRIORITY_MAX; priority >= 0; priority--) {
		if (sched_priority_is_band_top((uint16_t)priority)) continue;

		thread_t thread;

		while ((thread = run_queue_dequeue_priority(&processor->runq, (uint16_t)priority)) != 0) {
			sched_mlfq_assign(thread, SCHED_MLFQ_TOP_LEVEL);

			if (!run_queue_enqueue(&processor->runq, thread, RUN_QUEUE_TAIL)) {
				sched_fatal("run_queue_enqueue: MLFQ boost re-enqueue failed");
			}
		}
	}

	thread_t current = processor->active_thread;

	if (current != 0 && !thread_is_idle(current)) {
		sched_mlfq_assign(current, SCHED_MLFQ_TOP_LEVEL);
	}
}

/*
 * sched_starvation_scan
 *
 * The guard strict bands need: a thread that has waited on `runq` for
 * SCHED_STARVATION_TICKS or more is lifted to the top priority for one
 * top-level quantum, so even background work runs at least about twice a
 * second however busy the bands above it are. Its own band comes back the
 * moment the quantum runs out (the demotion reassigns it) or it is queued
 * again (sched_enqueue_on). Buckets are FIFO, so only their heads can be the
 * longest waiting. Returns how many were lifted. Called with the run
 * queue's lock held.
 */
static uint32_t
sched_starvation_scan(run_queue_t *runq, uint32_t now)
{
	uint32_t lifted = 0U;

	for (uint16_t priority = 0U; priority < SCHED_STARVATION_EMERGENCY_PRIORITY && lifted < SCHED_STARVATION_LIFT_MAX;
		 priority++) {
		thread_t thread;

		while (lifted < SCHED_STARVATION_LIFT_MAX &&
			   (thread = run_queue_dequeue_priority(runq, priority)) != 0) {
			uint32_t waited = now - thread->runq_enqueued_tick;
			uint16_t lift = waited >= SCHED_STARVATION_EMERGENCY_TICKS ? SCHED_STARVATION_EMERGENCY_PRIORITY
						  : waited >= SCHED_STARVATION_TICKS		   ? SCHED_STARVATION_PRIORITY
																	   : 0U;

			if (lift <= priority) {
				/*
				 * Not starved (or already that high): back where it was, at
				 * the head of its line, and still waiting since when it was
				 * queued. The enqueue stamps the time afresh; keeping that
				 * stamp would restart the wait of the thread at the head of
				 * each line at every scan, and the one that has waited longest
				 * could never be lifted.
				 */
				uint32_t queued_at = thread->runq_enqueued_tick;

				if (!run_queue_enqueue(runq, thread, RUN_QUEUE_HEAD))
					sched_fatal("run_queue_enqueue: starvation scan re-enqueue failed");
				thread->runq_enqueued_tick = queued_at;
				break;
			}

			thread->qos_promoted = true;
			thread->sched_pri = lift;
			thread->quantum_remaining = sched_mlfq_quantum[SCHED_MLFQ_TOP_LEVEL];
			if (!run_queue_enqueue(runq, thread, RUN_QUEUE_TAIL))
				sched_fatal("run_queue_enqueue: starvation lift enqueue failed");
			lifted++;
		}
	}
	return lifted;
}

bool
sched_task_set_qos(task_t task, uint8_t qos)
{
	if (task == 0 || qos >= NXU_QOS_COUNT) return false;

	/* Its threads take the band the next time they are queued or change
	 * level; one running now keeps its current priority until then. */
	__atomic_store_n(&task->qos, qos, __ATOMIC_RELEASE);
	return true;
}

/*
 * sched_quantum_reset
 *
 * The idle thread keeps a fixed placeholder quantum; it is never charged by
 * sched_tick() and never appears in the run queue. Every other thread's
 * quantum comes from its current MLFQ level.
 */
static void
sched_quantum_reset(thread_t thread)
{
	if (thread == 0) return;

	thread->quantum_remaining =
		thread_is_idle(thread) ? SCHED_DEFAULT_QUANTUM_TICKS : sched_mlfq_level_quantum(thread->mlfq_level);
}

/*
 * sched_queue_placement
 */
static run_queue_placement_t
sched_queue_placement(sched_queue_placement_t placement)
{
	return placement == SCHED_HEADQ ? RUN_QUEUE_HEAD : RUN_QUEUE_TAIL;
}

/*
 * sched_activate_thread
 *
 * Install the address-space and BSD process identity associated with the
 * incoming thread before its kernel context becomes current.
 */
static bool
sched_activate_thread(thread_t thread)
{
	if (thread == 0 || thread->task == 0) return false;

	task_t task = thread->task;

	/*
	 * Each CPU owns its own TTBR0: a user thread brings its task's address
	 * space up on the CPU that is about to run it, and a kernel thread leaves
	 * TTBR0 disabled, so a CPU that is idle or running kernel code never keeps
	 * a dead process's page tables reachable (vm_address_space_destroy waits
	 * for that). The process identity needs no switching: current_proc() is
	 * derived from the running thread.
	 */
	return task_is_kernel(task) ? vm_address_space_deactivate() : task_activate_address_space(task);
}

/*
 * sched_finish_switch
 *
 * Complete the handoff after the incoming stack is live. A terminating
 * outgoing thread can only be reaped at this point because its old EL1 stack
 * is no longer executing.
 */
static void
sched_finish_switch(void)
{
	uint64_t irq_state = ml_irq_save();
	processor_t processor = current_processor();

	/* previous_thread and previous_requeue are this CPU's own: written before the switch, read here. */
	thread_t previous = processor->previous_thread;
	bool requeue = processor->previous_requeue;

	processor->previous_thread = 0;
	processor->previous_requeue = false;

	if (previous == 0 || previous == current_thread()) {
		ml_irq_restore(irq_state);
		return;
	}

	/* Publishing on_cpu=0 lets teardown reap the stack and drop its final
	 * reference. Keep the object resident until this CPU finishes reading it,
	 * or a new thread can reuse the slot under the switch footer. */
	if (!thread_reference(previous)) sched_fatal("sched_finish_switch: outgoing thread has no residency reference");

	/*
	 * The previous thread's context was saved before the switch and this CPU
	 * is off its stack, so from here another CPU may run it. Release it, and
	 * pick up a wakeup that arrived while it was still switching out (see
	 * thread_setrun): that wakeup is what would otherwise have queued it.
	 */
	nxu_spin_lock(&previous->sched_lock);
	__atomic_store_n(&previous->on_cpu, (struct processor *)0, __ATOMIC_RELEASE);

	bool woken = previous->wakeup_deferred;

	previous->wakeup_deferred = false;
	nxu_spin_unlock(&previous->sched_lock);

	if (thread_is_terminated(previous)) {
		(void)thread_reap(previous);
	} else if ((requeue || woken) && !thread_is_idle(previous) && thread_is_runnable(previous) && previous->runq == 0) {
		processor_t target = sched_select_cpu(previous);

		if (!sched_enqueue_on(target, previous, RUN_QUEUE_TAIL)) {
			sched_fatal("sched_finish_switch: could not requeue the outgoing thread");
		}
	}

	thread_deallocate(previous);
	ml_irq_restore(irq_state);
}

/*
 * sched_thread_continue
 *
 * First C instruction executed by a never-run thread after its synthetic
 * machine context is restored.
 */
static __attribute__((noreturn)) void
sched_thread_continue(void)
{
	sched_finish_switch();

	thread_t thread = current_thread();

	if (thread == 0) sched_fatal("sched_thread_continue: first thread has no current object");

	if ((thread->flags & TH_FLAG_KERNEL) != 0U) {
		thread_continue_t continuation = thread->continuation;
		void *parameter = thread->parameter;

		if (continuation == 0) {
			sched_fatal("sched_thread_continue: kernel thread has no continuation");
		}

		ml_irq_enable();
		continuation(parameter);

		if (!sched_thread_terminate(thread)) {
			sched_fatal("sched_thread_terminate: returned kernel thread could not terminate");
		}

		sched_exit_current();
	}

	if (!machine_thread_has_user_state(&thread->machine)) {
		sched_fatal("machine_thread_has_user_state: user thread has no EL0 state");
	}

	machine_thread_enter_user(&thread->machine);

	/*
	 * A thread leaves EL0 for good only when it has been terminated: by its own
	 * exit, or by another CPU that killed its process and is now waiting for it
	 * to get here (exception_handle unwinds a terminated thread). Anything else
	 * returning to this point is an invalid scheduler transition.
	 *
	 * "Terminated" is the test, not "no longer active": thread_terminate() sets
	 * the state and then clears `active` under the thread lock, and this thread
	 * can notice the first and get here before the second is visible to it.
	 */
	if (!thread_is_terminated(thread)) {
		sched_fatal("thread_is_terminated: user thread left EL0 without being terminated");
	}

	sched_exit_current();
}

/*
 * sched_prepare_new_thread
 */
static bool
sched_prepare_new_thread(thread_t thread)
{
	if (thread == 0 || thread_is_bootstrap(thread)) return thread != 0;

	if (thread->kernel_stack == 0 && !thread_stack_alloc(thread)) return false;

	return machine_thread_prepare_context(&thread->machine, thread_kernel_stack_top(thread),
										  (uint64_t)sched_thread_continue);
}

/*
 * sched_idle_loop
 *
 * What every CPU's idle thread does: sleep until an interrupt, and switch to
 * work at the interrupt's return (i386_trap_exit and arm64's equivalent call
 * sched_preempt at the idle thread's safe point).
 *
 * Work can also be waiting with no interrupt on its way. sched_switch can
 * land here with preemption_pending set, for a thread queued while it had the
 * run queue unlocked (see the comment where it recomputes the flag): whoever
 * queued it saw the flag already set and sent no IPI. A busy CPU's request to
 * pull work (balance_requested) can likewise have arrived while this CPU was
 * still busy. With a periodic tick the next tick's return path took care of
 * both within a tick; an idle CPU on a deadline event timer takes no ticks,
 * so it must look before it sleeps. The look and the sleep are one step with
 * interrupts masked (cpu_idle_halt), so anything that arrives after the look
 * wakes the sleep.
 */
static __attribute__((noreturn)) void
sched_idle_loop(void)
{
	for (;;) {
		ml_irq_disable();

		processor_t processor = current_processor();

		if (__atomic_load_n(&processor->preemption_pending, __ATOMIC_ACQUIRE) ||
			__atomic_load_n(&processor->balance_requested, __ATOMIC_ACQUIRE)) {
			if (!sched_preempt()) sched_clear_preemption();

			ml_irq_enable();
			continue;
		}

		cpu_idle_halt();
	}
}

/*
 * sched_idle_continue
 */
static __attribute__((noreturn)) void
sched_idle_continue(void *parameter)
{
	(void)parameter;

	sched_idle_loop();
}

/*
 * sched_cpu_may_run
 *
 * Whether `thread` may run on `cpu`. A thread inside kernel code that assumes a
 * single CPU (sched_bind_boot_cpu) runs on the boot CPU whatever its affinity
 * says; otherwise its affinity decides. Which CPUs a new thread starts with is
 * decided at creation (processor_default_affinity and the user default).
 */
static bool
sched_cpu_may_run(thread_t thread, uint32_t cpu)
{
	if (__atomic_load_n(&thread->legacy_depth, __ATOMIC_ACQUIRE) != 0U) return cpu == 0U;

	return cpuset_contains(&thread->affinity, cpu);
}

/*
 * sched_bind_boot_cpu / sched_unbind_boot_cpu
 *
 * The compatibility mechanism for kernel code that was written for one CPU (the
 * VFS and filesystems, the drivers, process creation and exec): a thread that is
 * about to call it moves to the boot CPU first and stays there until it is done.
 * On the boot CPU kernel code runs to completion or to an explicit sleep, exactly
 * as it always did, so every assumption those subsystems make still holds without
 * a lock: no two threads can be inside them at once except at a sleep, and device
 * interrupts arrive on that same CPU.
 *
 * The move is an ordinary scheduling switch (it may take the thread to another
 * CPU's run queue and back), so the caller must hold no spinlock and not have
 * cached this CPU's identity. It nests. Binding costs nothing on the boot CPU.
 */
void
sched_bind_boot_cpu(void)
{
	thread_t thread = current_thread();

	if (thread == 0 || !g_sched.initialized) return;

	(void)__atomic_add_fetch(&thread->legacy_depth, 1U, __ATOMIC_ACQ_REL);

	/* sched_switch sends a thread that may not stay where it is back to its allowed CPU. */
	while (current_processor()->cpu_id != 0U) {
		if (!sched_yield()) break;
	}
}

void
sched_unbind_boot_cpu(void)
{
	thread_t thread = current_thread();

	if (thread == 0 || !g_sched.initialized) return;

	if (__atomic_load_n(&thread->legacy_depth, __ATOMIC_ACQUIRE) != 0U) {
		(void)__atomic_sub_fetch(&thread->legacy_depth, 1U, __ATOMIC_ACQ_REL);
	}
}

/*
 * sched_select_cpu
 *
 * Choose the CPU a runnable thread is queued on. In order of preference:
 *
 *   1. the CPU it last ran on, when that is idle (cache-warm and free),
 *   2. otherwise the least loaded CPU it may run on (an idle one, if any),
 *      unless the previous CPU is nearly as loaded, in which case it stays
 *      where it was (moving a thread to save one queue place costs more than
 *      it gains).
 *
 * Only online CPUs the thread's affinity and kind allow are considered; the
 * boot CPU is the fallback when none is. The common case (a pinned thread, or
 * an idle previous CPU) does not scan. Loads are read without locks, so the
 * choice is a hint: the enqueue that follows is what is made atomic.
 */
static processor_t
sched_select_cpu(thread_t thread)
{
	uint32_t count = processor_count();
	processor_t last = processor_by_id(thread->last_cpu);
	bool last_ok = last != 0 && processor_is_online(last) && sched_cpu_may_run(thread, last->cpu_id);

	if (last_ok && processor_load(last) == 0U) return last;

	processor_t best = 0;
	uint32_t best_load = UINT32_MAX;

	for (uint32_t cpu = 0U; cpu < count; cpu++) {
		processor_t candidate = processor_by_id(cpu);

		if (candidate == 0 || !processor_is_online(candidate) || !sched_cpu_may_run(thread, cpu)) continue;

		uint32_t load = processor_load(candidate);

		if (load < best_load) {
			best = candidate;
			best_load = load;
		}
	}
	if (best == 0) return processor_by_id(0);

	if (last_ok && best_load != 0U && processor_load(last) <= best_load + 1U) return last;

	return best;
}

/*
 * sched_enqueue_on
 *
 * Put a runnable thread that is on no queue and on no CPU onto `target`'s run
 * queue, and make the target notice if it should now run something else. The
 * target may be another CPU: this is the one place a CPU takes another's
 * runq_lock to add work.
 *
 * The reschedule is requested by setting the target's preemption_pending flag
 * (its need_resched) and, when the target is not this CPU, a reschedule IPI: an
 * idle CPU is asleep in WFI and a busy one is between its own safe points. The
 * IPI is only sent when the flag was not already set, so a burst of wakeups
 * costs one interrupt.
 */
static bool
sched_enqueue_on(processor_t target, thread_t thread, run_queue_placement_t placement)
{
	processor_t self = current_processor();
	uint64_t irq_state = ml_irq_save();
	bool kick = false;

	nxu_spin_lock(&target->runq_lock);

	/*
	 * Queued afresh (woken, started, yielded): back to its own band at its
	 * level, which also ends a starvation lift and takes in a QoS change.
	 */
	if (thread->runq == 0 && !thread_is_idle(thread)) {
		uint16_t priority = sched_qos_priority(sched_thread_qos(thread), thread->mlfq_level);

		thread->base_pri = priority;
		if (thread->yield_depressed && priority > SCHED_YIELD_DEPRESS_PRIORITY) priority = SCHED_YIELD_DEPRESS_PRIORITY;
		thread->qos_promoted = false;
		thread->sched_pri = priority;
	}

	bool queued = thread->runq == 0 && run_queue_enqueue(&target->runq, thread, placement);

	if (queued) {
		thread_t active = target->active_thread;

		if (thread->last_cpu != target->cpu_id) target->migrate_in_count++;

		if (active == 0 || thread_is_idle(active) || thread->sched_pri > active->sched_pri) {
			kick = !__atomic_exchange_n(&target->preemption_pending, true, __ATOMIC_ACQ_REL);
		}
	}

	nxu_spin_unlock(&target->runq_lock);

	if (kick && target != self) ipi_send(target->cpu_id, IPI_RESCHEDULE);

	ml_irq_restore(irq_state);
	return queued;
}

/*
 * sched_abandon
 *
 * A thread this CPU took off its run queue turned out not to be runnable (it was
 * terminated or held in that instant): release it without running it. Anything
 * that tried to make it runnable in the meantime was deferred onto it (it was
 * still "on" this CPU), so that wakeup is carried out now unless the thread is
 * gone. The caller holds no lock.
 */
static void
sched_abandon(thread_t thread)
{
	nxu_spin_lock(&thread->sched_lock);
	__atomic_store_n(&thread->on_cpu, (struct processor *)0, __ATOMIC_RELEASE);

	bool woken = thread->wakeup_deferred;

	thread->wakeup_deferred = false;
	nxu_spin_unlock(&thread->sched_lock);

	if (woken && !thread_is_terminated(thread) && thread_is_runnable(thread) && thread->runq == 0) {
		(void)sched_enqueue_on(sched_select_cpu(thread), thread, RUN_QUEUE_TAIL);
	}
}

/*
 * sched_switch
 *
 * Select and activate an incoming thread, then switch SP_EL1 and the AArch64
 * callee-saved context. IRQs remain masked across the scheduler handoff so
 * processor->active_thread can never disagree with the stack actually in use.
 *
 * Only this CPU's run queue is touched, under its own lock. The outgoing thread
 * is not queued here even when it yielded: until the switch below has saved its
 * context another CPU must not be able to pick it up, so it is handed to
 * sched_finish_switch (previous_requeue), which queues it from the incoming
 * thread's stack.
 */
static bool
sched_switch(sched_switch_reason_t reason)
{
	if (!g_sched.initialized) return false;

	uint64_t irq_state = ml_irq_save();
	processor_t processor = current_processor();

	if (processor == 0) {
		ml_irq_restore(irq_state);
		return false;
	}

	nxu_spin_lock(&processor->runq_lock);

	thread_t current = processor->active_thread;

	if (current == 0) {
		nxu_spin_unlock(&processor->runq_lock);
		ml_irq_restore(irq_state);
		return false;
	}

	if (reason == SCHED_SWITCH_BLOCK) sched_mlfq_blocked(current);

	bool yielding = reason == SCHED_SWITCH_YIELD || reason == SCHED_SWITCH_PREEMPT;

	bool requeue_current =
		yielding && thread_is_active(current) && thread_is_runnable(current) && !thread_is_idle(current);

	/*
	 * An explicit yield by a thread that could go on running: depress it, so
	 * the threads of the bands below get the CPU (see yield_depressed). Its
	 * own band comes back when it blocks.
	 */
	if (reason == SCHED_SWITCH_YIELD && requeue_current) {
		if (current->yield_streak != UINT16_MAX) current->yield_streak++;
		if (current->yield_streak >= SCHED_YIELD_DEPRESS_STREAK && current->sched_pri > SCHED_YIELD_DEPRESS_PRIORITY) {
			current->yield_depressed = true;
			current->sched_pri = SCHED_YIELD_DEPRESS_PRIORITY;
		}
	}

	thread_t candidate = run_queue_peek(&processor->runq);
	bool stay = false;

	if (yielding) {
		if (!requeue_current) {
			stay = candidate == 0;
		} else {
			/*
			 * The current thread keeps the CPU when nothing queued here is at
			 * least as good, unless its affinity was narrowed to exclude this
			 * CPU, in which case it has to leave.
			 */
			stay = sched_cpu_may_run(current, processor->cpu_id) &&
				   (candidate == 0 || candidate->sched_pri < current->sched_pri);
		}
	}

	if (stay) {
		__atomic_store_n(&processor->preemption_pending, false, __ATOMIC_RELEASE);
		sched_quantum_reset(current);
		nxu_spin_unlock(&processor->runq_lock);
		ml_irq_restore(irq_state);
		return true;
	}

	thread_t next;

	for (;;) {
		next = run_queue_dequeue(&processor->runq);

		if (next == 0) next = processor->idle_thread;

		if (next == 0 || next == current) {
			nxu_spin_unlock(&processor->runq_lock);
			ml_irq_restore(irq_state);
			return next == current;
		}

		/*
		 * `next` is off every queue now, so nobody else can choose it; marking it
		 * on this CPU before the lock is dropped keeps a concurrent wakeup from
		 * queueing it elsewhere (see thread_setrun).
		 */
		nxu_spin_lock(&next->sched_lock);
		__atomic_store_n(&next->on_cpu, processor, __ATOMIC_RELEASE);
		nxu_spin_unlock(&next->sched_lock);

		next->last_cpu = processor->cpu_id;
		processor->next_thread = next;
		processor->dispatch_count++;

		nxu_spin_unlock(&processor->runq_lock);

		bool ready = sched_activate_thread(next);

		nxu_spin_lock(&processor->runq_lock);

		/*
		 * Another CPU can terminate or hold a thread between the moment this
		 * one took it off the queue and the moment it became current here (it
		 * looks for the thread on its run queue, which it has just left). A
		 * thread that is no longer runnable is given up, not run; a thread that
		 * was only held is queued again when it is released.
		 */
		if (ready && thread_set_current(next)) break;

		nxu_spin_unlock(&processor->runq_lock);

		if (!ready && !thread_is_terminated(next)) {
			sched_fatal("sched_activate_thread: incoming thread activation failed");
		}

		sched_abandon(next);
		nxu_spin_lock(&processor->runq_lock);
	}

	processor->previous_thread = current;
	processor->previous_requeue = requeue_current;
	processor->next_thread = 0;

	__atomic_store_n(&processor->state, thread_is_idle(next) ? PROCESSOR_IDLE : PROCESSOR_RUNNING, __ATOMIC_RELEASE);

	/*
	 * The flag is recomputed from the queue, not cleared. The run queue lock was
	 * dropped while the address space was switched, and another CPU can have queued
	 * a thread here in that window: it found the flag already set (by the
	 * interrupt that got this switch going) and so sent no interrupt of its own,
	 * because the flag is what says "an interrupt is already on its way". Clearing
	 * it now would throw the only record of that thread away, and an idle CPU
	 * would sleep with work queued for good.
	 */
	thread_t waiting = run_queue_peek(&processor->runq);

	__atomic_store_n(&processor->preemption_pending,
					 waiting != 0 && (thread_is_idle(next) || waiting->sched_pri > next->sched_pri), __ATOMIC_RELEASE);

	processor->context_switch_count++;

	if (reason == SCHED_SWITCH_PREEMPT) {
		processor->preemption_count++;
	}

	/* How many CPUs are running threads of user processes right now, and the most there ever were. */
	bool leaving_user = sched_thread_is_user(current);
	bool entering_user = sched_thread_is_user(next);

	if (entering_user) processor->user_dispatch_count++;

	if (entering_user && !leaving_user) {
		uint32_t running = __atomic_add_fetch(&g_sched_user_running, 1U, __ATOMIC_ACQ_REL);
		uint32_t peak = __atomic_load_n(&g_sched_user_peak, __ATOMIC_RELAXED);

		while (running > peak && !__atomic_compare_exchange_n(&g_sched_user_peak, &peak, running, false,
															  __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
		}
	} else if (leaving_user && !entering_user) {
		(void)__atomic_sub_fetch(&g_sched_user_running, 1U, __ATOMIC_ACQ_REL);
	}

	sched_quantum_reset(next);

	/* Measured CPU time (thread->run_us, processor->busy_us). */
	nxu_spin_lock(&current->accounting_lock);
	uint64_t now_us = timer_get_microseconds();
	if (!thread_is_idle(current) && current->dispatched_us != 0ULL && now_us > current->dispatched_us) {
		current->run_us += now_us - current->dispatched_us;
	}

	current->dispatched_us = 0ULL;
	nxu_spin_unlock(&current->accounting_lock);

	if (processor->busy_since_us != 0ULL && now_us > processor->busy_since_us) {
		processor->busy_us += now_us - processor->busy_since_us;
	}

	nxu_spin_lock(&next->accounting_lock);
	next->dispatched_us = now_us;
	nxu_spin_unlock(&next->accounting_lock);
	processor->busy_since_us = thread_is_idle(next) ? 0ULL : now_us;

	nxu_spin_unlock(&processor->runq_lock);

	/* The scheduler's tick follows the CPU: one while it runs a thread, none while it idles (sched_timer.c). */
	if (thread_is_idle(current) != thread_is_idle(next)) sched_timer_dispatch(processor, thread_is_idle(next), now_us);

	machine_thread_switch_context(&current->machine, &next->machine);

	/*
	 * This instruction executes when this thread is selected again. A
	 * never-run incoming thread instead starts at sched_thread_continue().
	 */
	sched_finish_switch();
	ml_irq_restore(irq_state);
	return true;
}

/*
 * sched_bootstrap
 */
bool
sched_bootstrap(task_t kernel_task)
{
	if (g_sched.initialized) return true;

	if (kernel_task == 0 || !task_is_kernel(kernel_task)) return false;

	if (!processor_bootstrap()) return false;

	/* Timed waits enter timer calls from the first tick on; their queues must exist before it. */
	timer_call_init();

	memset(&g_sched, 0, sizeof(g_sched));

	thread_t bootstrap_thread;

	if (!thread_create_bootstrap(kernel_task, &bootstrap_thread)) return false;

	if (!thread_set_current(bootstrap_thread)) {
		thread_deallocate(bootstrap_thread);
		return false;
	}

	thread_t idle_thread;

	if (!kernel_thread_create(kernel_task, sched_idle_continue, 0, &idle_thread)) {
		thread_deallocate(bootstrap_thread);
		return false;
	}

	if (!thread_set_idle(idle_thread) || !thread_stack_alloc(idle_thread) ||
		!machine_thread_prepare_context(&idle_thread->machine, thread_kernel_stack_top(idle_thread),
										(uint64_t)sched_thread_continue) ||
		!thread_start(idle_thread)) {
		(void)thread_terminate(idle_thread);
		(void)thread_reap(idle_thread);
		thread_deallocate(idle_thread);
		thread_deallocate(bootstrap_thread);
		return false;
	}

	sched_mlfq_assign(bootstrap_thread, SCHED_MLFQ_TOP_LEVEL);
	sched_quantum_reset(idle_thread);

	processor_t processor = current_processor();

	processor->active_thread = bootstrap_thread;
	processor->idle_thread = idle_thread;
	processor->next_thread = 0;
	processor->previous_thread = 0;
	processor->previous_requeue = false;
	processor->state = PROCESSOR_RUNNING;

	/*
	 * The boot thread carries the boot CPU's identity (the UI session, EL0
	 * excursions parked on its stack, the boot-time kernel state): it stays on
	 * the boot CPU whatever the default affinity is.
	 */
	cpuset_clear(&bootstrap_thread->affinity);
	cpuset_add(&bootstrap_thread->affinity, processor->cpu_id);

	bootstrap_thread->on_cpu = processor;
	bootstrap_thread->last_cpu = processor->cpu_id;

	g_sched.bootstrap_thread = bootstrap_thread;
	g_sched.kernel_task = kernel_task;
	g_sched.initialized = true;

	g_sched_qos_enabled = boot_arg_bool("sched.qos", true);
	kprintf("quality-of-service bands %s (sched.qos=0 turns them off)\n", g_sched_qos_enabled ? "on" : "off");
	kprintf("standard timeslicing quantum is %u tick(s), %u MLFQ levels boosted every %u tick(s)\n",
			(uint32_t)SCHED_DEFAULT_QUANTUM_TICKS, (uint32_t)SCHED_MLFQ_LEVELS, (uint32_t)SCHED_MLFQ_BOOST_INTERVAL_TICKS);

	return true;
}

/*
 * sched_cpu_prepare
 *
 * Boot CPU, before it starts secondary `cpu` (whose processor is registered):
 * give it the thread that represents the context it will boot into, which is
 * also its idle thread. That context is the CPU's boot stack, so its idle loop
 * (sched_cpu_idle) runs there and the CPU needs no other stack for idling.
 */
bool
sched_cpu_prepare(uint32_t cpu)
{
	processor_t processor = processor_by_id(cpu);

	if (!g_sched.initialized || processor == 0 || processor->idle_thread != 0) return false;

	thread_t idle;

	if (!thread_create_cpu_idle(g_sched.kernel_task, cpu, &idle)) return false;

	sched_quantum_reset(idle);

	idle->on_cpu = processor;
	processor->idle_thread = idle;
	processor->active_thread = idle;
	return true;
}

/*
 * sched_cpu_idle
 *
 * The end of a secondary CPU's start-up: from here it is an ordinary scheduling
 * CPU, idling on the stack it booted on. Timer, reschedule IPI and device
 * interrupts wake it; when work has been queued on it the interrupt's return
 * path (sched_preempt, at the idle thread's safe point) switches to that work,
 * and the CPU comes back to this loop when it runs out.
 */
__attribute__((noreturn)) void
sched_cpu_idle(void)
{
	processor_t processor = current_processor();

	__atomic_store_n(&processor->state, PROCESSOR_IDLE, __ATOMIC_RELEASE);

	sched_idle_loop();
}

bool
sched_is_initialized(void)
{
	return g_sched.initialized;
}

thread_t
sched_bootstrap_thread(void)
{
	return g_sched.initialized ? g_sched.bootstrap_thread : 0;
}

thread_t
sched_idle_thread(void)
{
	processor_t processor = g_sched.initialized ? current_processor() : 0;

	return processor != 0 ? processor->idle_thread : 0;
}

/*
 * sched_kick_thread
 *
 * Make the CPU that is running `thread` take an interrupt now, so that a state
 * change made to the thread (it was terminated, a signal is pending for it) is
 * noticed at the next return to user mode instead of waiting for a timer tick.
 * Nothing to do if it is not running anywhere, or is running here.
 */
void
sched_kick_thread(thread_t thread)
{
	if (thread == 0) return;

	processor_t target = __atomic_load_n(&thread->on_cpu, __ATOMIC_ACQUIRE);

	if (target != 0 && target != current_processor()) ipi_send(target->cpu_id, IPI_RESCHEDULE);
}

/*
 * sched_wait_thread_off_cpu
 *
 * Wait until `thread` is no longer live on any CPU (its context saved, its CPU
 * off its stack). The caller has terminated it, so it is on its way out: a user
 * thread reaches the exit path at its next exception (kicked with an IPI every
 * half millisecond) and a kernel thread finishes its current call. Holds no lock
 * and must not be called for the calling thread. Returns false if it is still
 * running after about five seconds, which means something is stuck and the
 * thread's resources must not be freed.
 */
bool
sched_wait_thread_off_cpu(thread_t thread)
{
	if (thread == 0) return true;

	uint64_t deadline = timer_get_microseconds() + 5000000ULL;
	uint64_t next_kick = 0ULL;

	while (__atomic_load_n(&thread->on_cpu, __ATOMIC_ACQUIRE) != 0) {
		uint64_t now = timer_get_microseconds();

		if (now > deadline) return false;

		if (now >= next_kick) {
			next_kick = now + 500ULL;
			sched_kick_thread(thread);
		}

		cpu_relax();
	}

	return true;
}

bool
sched_thread_can_run_on(thread_t thread, uint32_t cpu)
{
	return thread != 0 && !thread_is_idle(thread) && sched_cpu_may_run(thread, cpu);
}

/*
 * thread_setrun
 *
 * Make a runnable thread eligible to run: choose a CPU for it and queue it
 * there. A thread that is still switching out on some CPU (on_cpu) is not
 * queued: it is marked, and the CPU that is switching away queues it once its
 * context is safely saved (sched_finish_switch). That is what makes waking a
 * thread from any CPU, at any moment, safe.
 */
bool
thread_setrun(thread_t thread, sched_queue_placement_t placement)
{
	if (!g_sched.initialized || thread == 0 || thread_is_idle(thread) || !thread_is_runnable(thread) ||
		(placement != SCHED_HEADQ && placement != SCHED_TAILQ)) {
		return false;
	}

	uint64_t irq_state = ml_irq_save();

	if (thread == current_thread()) {
		ml_irq_restore(irq_state);
		return false;
	}

	nxu_spin_lock(&thread->sched_lock);

	if (__atomic_load_n(&thread->on_cpu, __ATOMIC_ACQUIRE) != 0) {
		thread->wakeup_deferred = true;
		nxu_spin_unlock(&thread->sched_lock);
		ml_irq_restore(irq_state);
		return true;
	}

	nxu_spin_unlock(&thread->sched_lock);

	bool valid = sched_enqueue_on(sched_select_cpu(thread), thread, sched_queue_placement(placement));

	ml_irq_restore(irq_state);
	return valid;
}

/*
 * thread_run_queue_remove
 *
 * Take a queued thread off whichever CPU's run queue it is on. The queue is
 * found from the thread, so it can move between the lookup and the lock;
 * the check after the lock says whether it did, and the lookup is retried.
 */
bool
thread_run_queue_remove(thread_t thread)
{
	if (!g_sched.initialized || thread == 0) return false;

	uint64_t irq_state = ml_irq_save();
	bool result = false;

	for (;;) {
		run_queue_t *runq = __atomic_load_n(&thread->runq, __ATOMIC_ACQUIRE);

		if (runq == 0) break;

		processor_t owner = (processor_t)((uint8_t *)runq - offsetof(struct processor, runq));

		nxu_spin_lock(&owner->runq_lock);

		if (thread->runq == runq) {
			result = run_queue_remove(runq, thread);
			nxu_spin_unlock(&owner->runq_lock);
			break;
		}

		nxu_spin_unlock(&owner->runq_lock);
	}

	ml_irq_restore(irq_state);
	return result;
}

thread_t
thread_select(processor_t processor)
{
	if (!g_sched.initialized || processor == 0) return 0;

	uint64_t irq_state = nxu_spin_lock_irqsave(&processor->runq_lock);

	thread_t thread = run_queue_dequeue(&processor->runq);

	if (thread == 0) thread = processor->idle_thread;

	if (thread != 0) {
		processor->next_thread = thread;
		processor->dispatch_count++;
		sched_quantum_reset(thread);
	}

	nxu_spin_unlock_irqrestore(&processor->runq_lock, irq_state);
	return thread;
}

bool
sched_thread_start(thread_t thread)
{
	if (thread == 0 || thread->started) return false;

	if (!sched_prepare_new_thread(thread)) return false;

	/*
	 * Rule 3, the newbie premium: a brand-new thread is assumed to be
	 * interactive until proven otherwise, so it starts in the
	 * highest-priority MLFQ queue.
	 */
	sched_mlfq_assign(thread, SCHED_MLFQ_TOP_LEVEL);

	if (!thread_start(thread)) return false;

	if (thread_is_idle(thread)) return true;

	return thread_setrun(thread, SCHED_TAILQ);
}

bool
sched_thread_wait(thread_t thread, bool uninterruptible)
{
	if (thread == 0) return false;

	if (thread->runq != 0 && !thread_run_queue_remove(thread)) return false;

	return thread_wait(thread, uninterruptible);
}

bool
sched_thread_wakeup(thread_t thread)
{
	if (thread == 0 || !thread_go(thread)) return false;

	if (thread_is_runnable(thread) && !thread_is_idle(thread)) {
		return thread_setrun(thread, SCHED_TAILQ);
	}

	return true;
}

bool
sched_thread_hold(thread_t thread)
{
	if (thread == 0) return false;

	if (thread->runq != 0 && !thread_run_queue_remove(thread)) return false;

	return thread_hold(thread);
}

bool
sched_thread_release(thread_t thread)
{
	if (thread == 0 || !thread_release(thread)) return false;

	if (thread_is_runnable(thread) && !thread_is_idle(thread)) {
		return thread_setrun(thread, SCHED_TAILQ);
	}

	return true;
}

bool
sched_thread_terminate(thread_t thread)
{
	if (thread == 0) return false;

	/* Before thread_terminate clobbers the links they share with the run
	 * queue: a sleeping thread must leave its wait queue first. */
	(void)waitq_remove(thread);

	/* And the list of timed waiters, which would otherwise keep pointing at it after it is reclaimed. */
	waitq_timer_cancel(thread);

	/*
	 * A queued thread has to be off its queue before it can terminate, and
	 * another CPU's balancer can put it on a different queue between the two
	 * steps; so try again until it is either terminated or already gone.
	 */
	for (uint32_t attempt = 0U; attempt < 64U; attempt++) {
		if (thread->runq != 0) (void)thread_run_queue_remove(thread);

		if (thread_terminate(thread)) return true;

		if (thread_is_terminated(thread) || !thread_is_active(thread)) return false;
	}

	return false;
}

bool
sched_thread_set_priority(thread_t thread, uint16_t priority)
{
	if (thread == 0 || priority < THREAD_PRIORITY_MIN || priority > THREAD_PRIORITY_MAX || thread_is_idle(thread)) {
		return false;
	}

	bool queued = thread->runq != 0;

	if (queued && !thread_run_queue_remove(thread)) return false;

	if (!thread_set_priority(thread, priority)) {
		if (queued) (void)thread_setrun(thread, SCHED_TAILQ);
		return false;
	}

	if (queued) return thread_setrun(thread, SCHED_TAILQ);

	/* A thread lowering its own priority may now be beaten by something queued on its CPU. */
	uint64_t irq_state = nxu_spin_lock_irqsave(&current_processor()->runq_lock);
	processor_t processor = current_processor();

	thread_t candidate = run_queue_peek(&processor->runq);

	if (thread == processor->active_thread && candidate != 0 && candidate->sched_pri > thread->sched_pri) {
		__atomic_store_n(&processor->preemption_pending, true, __ATOMIC_RELEASE);
	}

	nxu_spin_unlock_irqrestore(&processor->runq_lock, irq_state);
	return true;
}

bool
sched_yield(void)
{
	return sched_switch(SCHED_SWITCH_YIELD);
}

bool
sched_block_commit(void)
{
	return sched_switch(SCHED_SWITCH_BLOCK);
}

bool
sched_block(bool uninterruptible)
{
	thread_t thread = current_thread();

	if (thread == 0 || thread_is_idle(thread) || !sched_thread_wait(thread, uninterruptible)) {
		return false;
	}

	return sched_block_commit();
}

__attribute__((noreturn)) void
sched_exit_current(void)
{
	thread_t thread = current_thread();

	if (thread == 0 || !thread_is_terminated(thread)) {
		sched_fatal("thread_is_terminated: exit requested by non-terminated thread");
	}

	if (!sched_switch(SCHED_SWITCH_EXIT)) {
		sched_fatal("sched_switch: terminating thread could not dispatch successor");
	}

	sched_fatal("sched_exit_current: terminated thread resumed unexpectedly");
}

/*
 * sched_migrate_one
 *
 * Move one RUNNABLE thread from `from`'s run queue to `to`'s, for load
 * balancing. Only a thread that is queued moves: a running thread is never
 * migrated (its context is live on its CPU; it can only leave by being switched
 * out and queued anew), and a thread that is mid-switch is not queued at all.
 * Both queues are locked, lower logical CPU id first, so two CPUs balancing
 * towards each other cannot deadlock. The thread taken is the lowest-priority
 * one that may run on `to`: it is the one whose delay hurts least and whose
 * cache is coldest, and the higher levels keep their interactive latency.
 */
static bool
sched_migrate_one(processor_t from, processor_t to)
{
	processor_t first = from->cpu_id < to->cpu_id ? from : to;
	processor_t second = first == from ? to : from;

	nxu_spin_lock(&first->runq_lock);
	nxu_spin_lock(&second->runq_lock);

	thread_t victim = 0;

	for (uint32_t priority = 0U; priority < RUN_QUEUE_COUNT && victim == 0; priority++) {
		if ((from->runq.bitmap & (1ULL << priority)) == 0ULL) continue;

		for (thread_t thread = from->runq.queues[priority].head; thread != 0; thread = thread->sched_links.runq.next) {
			if (sched_cpu_may_run(thread, to->cpu_id)) {
				victim = thread;
				break;
			}

		}
	}

	bool moved = false;

	if (victim != 0 && run_queue_remove(&from->runq, victim)) {
		moved = run_queue_enqueue(&to->runq, victim, RUN_QUEUE_TAIL);

		if (!moved) sched_fatal("sched_migrate_one: could not queue a migrating thread");

		from->migrate_out_count++;
		to->migrate_in_count++;
		__atomic_store_n(&to->preemption_pending, true, __ATOMIC_RELEASE);
	}

	nxu_spin_unlock(&second->runq_lock);
	nxu_spin_unlock(&first->runq_lock);
	return moved;
}

/*
 * sched_balance
 *
 * Pull-style balancing, run from a CPU's own tick every few ticks: if this CPU
 * has less to do than the busiest CPU (idle: at least one queued thread
 * elsewhere; otherwise: two more than it has), take one thread from it.
 * Placement at wakeup (sched_select_cpu) does most of the spreading; this
 * catches what it cannot, such as several threads woken onto one CPU while
 * the others were busy, or work finished elsewhere.
 *
 * It scans all CPUs, so it runs at a low rate and only from the tick, never on
 * the wakeup or switch paths. The loads it reads are estimates; the move itself
 * is decided under the two run queue locks.
 */
static void
sched_balance(processor_t self)
{
	uint32_t count = processor_count();

	if (count <= 1U) return;

	uint32_t own_load = processor_load(self);
	processor_t busiest = 0;
	uint32_t busiest_load = 0U;

	for (uint32_t cpu = 0U; cpu < count; cpu++) {
		processor_t candidate = processor_by_id(cpu);

		if (candidate == self || !processor_is_online(candidate)) continue;

		uint32_t load = processor_load(candidate);

		if (load > busiest_load) {
			busiest = candidate;
			busiest_load = load;
		}
	}
	if (busiest == 0 || busiest->runq.count == 0U) return;

	if (busiest_load < own_load + (own_load == 0U ? 1U : 2U)) return;

	(void)sched_migrate_one(busiest, self);
}

/*
 * sched_idle_helper_locked
 *
 * An idle CPU that may run one of the threads waiting on
 * `self`'s run queue (self's runq_lock held), or 0. Scans the queue only when
 * something waits and some CPU is idle; most ticks return at the first test.
 */
static processor_t
sched_idle_helper_locked(processor_t self)
{
	if (self->runq.count == 0U) return 0;

	uint32_t count = processor_count();

	for (uint32_t cpu = 0U; cpu < count; cpu++) {
		processor_t candidate = processor_by_id(cpu);

		if (candidate == 0 || candidate == self || !processor_is_online(candidate)) continue;

		if (__atomic_load_n(&candidate->state, __ATOMIC_ACQUIRE) != PROCESSOR_IDLE) continue;

		for (uint32_t priority = 0U; priority < RUN_QUEUE_COUNT; priority++) {
			if ((self->runq.bitmap & (1ULL << priority)) == 0ULL) continue;

			for (thread_t thread = self->runq.queues[priority].head; thread != 0;
				 thread = thread->sched_links.runq.next) {
				if (sched_cpu_may_run(thread, cpu)) return candidate;
			}
		}
	}

	return 0;
}

void
sched_tick(void)
{
	if (!g_sched.initialized) return;

	uint64_t irq_state = ml_irq_save();
	processor_t processor = current_processor();

	nxu_spin_lock(&processor->runq_lock);

	thread_t thread = processor->active_thread;

	processor->sched_ticks++;

	if (thread != 0 && thread_is_active(thread) && !thread_is_idle(thread)) {
		bool quantum_expired;

		/* CPU time: what Activity Monitor shows (see thread_snapshot). */
		nxu_spin_lock(&thread->accounting_lock);
		thread->run_ticks++;
		nxu_spin_unlock(&thread->accounting_lock);
		processor->busy_ticks++;

		/*
		 * Rule 4, the demotion: the thread ran through its whole quantum,
		 * or through its level's allotment across several yields, without
		 * blocking, so treat it as CPU-bound and drop it one MLFQ level.
		 */
		bool demoted = sched_mlfq_charge_tick(thread, &quantum_expired);

		if (quantum_expired) processor->quantum_expiration_count++;

		if (demoted) __atomic_store_n(&processor->preemption_pending, true, __ATOMIC_RELEASE);
	}

	/*
	 * Rule 5, the equalizer boost. This runs on wall-clock ticks regardless
	 * of which thread is active (including while the CPU is idle) so
	 * background work queued during a quiet period is not left waiting at
	 * the bottom the moment something else shows up. Each CPU boosts its own
	 * queue on its own ticks: the levels are per CPU, and no CPU needs another
	 * one's lock for it.
	 */
	processor->boost_ticks++;

	if (processor->boost_ticks >= SCHED_MLFQ_BOOST_INTERVAL_TICKS) {
		processor->boost_ticks = 0U;
		sched_mlfq_boost_locked(processor);
	}

	/* The guard that keeps strict QoS bands from starving the lower ones (waits measured on run_queue_clock). */
	if (processor->sched_ticks % SCHED_STARVATION_SCAN_TICKS == 0U &&
		sched_starvation_scan(&processor->runq, run_queue_clock()) != 0U) {
		__atomic_store_n(&processor->preemption_pending, true, __ATOMIC_RELEASE);
	}

	/*
	 * Idle CPUs take no ticks with a deadline event timer, so they no longer
	 * balance on their own: a CPU with threads waiting that an idle CPU may
	 * run asks that CPU to come and pull one.
	 */
	processor_t idle_helper = sched_timer_enabled() ? sched_idle_helper_locked(processor) : 0;

	nxu_spin_unlock(&processor->runq_lock);

	/*
	 * Asked again on every tick while the work waits: a request the idle CPU
	 * could not act on (it was busy for a moment when the IPI came) is not
	 * lost, and an idle CPU used to look on each of its own ticks anyway.
	 */
	if (idle_helper != 0) {
		__atomic_store_n(&idle_helper->balance_requested, true, __ATOMIC_RELEASE);
		ipi_send(idle_helper->cpu_id, IPI_RESCHEDULE);
	}

	/*
	 * With a periodic event timer the boot CPU's tick is also what runs the
	 * timer calls, which end timed waits: they work from the first one on.
	 */
	if (processor->cpu_id == 0U && !etimer_deadline_mode()) {
		__atomic_store_n(&g_sched_ticking, true, __ATOMIC_RELEASE);
		(void)timer_call_expire(0U, clock_uptime_us());
	}

	if (processor->ticks % SCHED_BALANCE_INTERVAL_TICKS == 0U) sched_balance(processor);

	ml_irq_restore(irq_state);
}

bool
sched_preempt(void)
{
	/*
	 * A busy CPU asked this idle one to pull work (sched_tick): the
	 * reschedule IPI that carried the request brings it here, at the idle
	 * thread's safe point, and a thread pulled here is switched to below.
	 */
	if (g_sched.initialized) {
		processor_t processor = current_processor();

		if (__atomic_load_n(&processor->balance_requested, __ATOMIC_ACQUIRE) &&
			__atomic_exchange_n(&processor->balance_requested, false, __ATOMIC_ACQ_REL)) {
			thread_t active = processor->active_thread;

			if (active != 0 && thread_is_idle(active)) sched_balance(processor);
		}
	}

	if (!sched_preemption_pending()) return true;

	return sched_switch(SCHED_SWITCH_PREEMPT);
}

bool
sched_preemption_pending(void)
{
	if (!g_sched.initialized) return false;

	processor_t processor = current_processor();
	return __atomic_load_n(&processor->preemption_pending, __ATOMIC_ACQUIRE);
}

void
sched_clear_preemption(void)
{
	if (!g_sched.initialized) return;

	processor_t processor = current_processor();
	__atomic_store_n(&processor->preemption_pending, false, __ATOMIC_RELEASE);
}

static void
sched_test_thread(struct thread *thread, uint16_t priority)
{
	memset(thread, 0, sizeof(*thread));
	thread->thread_id = (thread_id_t)priority + 1ULL;
	thread->ref_count = 1U;
	thread->state = TH_RUN;
	thread->active = true;
	thread->started = true;
	thread->sched_pri = priority;
	thread->base_pri = priority;
	thread->max_priority = THREAD_PRIORITY_MAX;
}

bool
sched_run_queue_self_test(void)
{
	run_queue_t runq;
	struct thread low;
	struct thread high_a;
	struct thread high_b;

	run_queue_init(&runq);
	sched_test_thread(&low, 8U);
	sched_test_thread(&high_a, 42U);
	sched_test_thread(&high_b, 42U);

	if (!run_queue_enqueue(&runq, &low, RUN_QUEUE_TAIL)) return false;

	if (!run_queue_enqueue(&runq, &high_a, RUN_QUEUE_TAIL)) return false;

	if (!run_queue_enqueue(&runq, &high_b, RUN_QUEUE_TAIL)) return false;

	if (!run_queue_validate(&runq)) return false;

	if (run_queue_peek(&runq) != &high_a) return false;

	if (run_queue_dequeue(&runq) != &high_a) return false;

	if (run_queue_dequeue(&runq) != &high_b) return false;

	if (run_queue_dequeue(&runq) != &low) return false;

	if (run_queue_dequeue(&runq) != 0) return false;

	return run_queue_validate(&runq);
}

/* A scheduler self-test check failed: say where, so a failure on a boot log can be found. */
#define SCHED_SELF_TEST_FAIL()                                \
	do {                                                      \
		kprintf("check at line %u failed\n", (uint32_t)__LINE__); \
		return false;                                         \
	} while (0)

/*
 * sched_mlfq_test_thread
 */
static void
sched_mlfq_test_thread(struct thread *thread, uint8_t level)
{
	memset(thread, 0, sizeof(*thread));
	thread->thread_id = (thread_id_t)level + 100ULL;
	thread->ref_count = 1U;
	thread->state = TH_RUN;
	thread->active = true;
	thread->started = true;
	thread->max_priority = THREAD_PRIORITY_MAX;

	sched_mlfq_assign(thread, level);
}

bool
sched_mlfq_self_test(void)
{
	struct processor test_processor;
	struct thread queued_mid;
	struct thread queued_bottom;
	struct thread running;
	struct thread hog;

	memset(&test_processor, 0, sizeof(test_processor));
	run_queue_init(&test_processor.runq);

	/* A thread starts life at the top queue (Rule 3). */
	sched_mlfq_test_thread(&hog, SCHED_MLFQ_TOP_LEVEL);
	if (hog.mlfq_level != SCHED_MLFQ_TOP_LEVEL) SCHED_SELF_TEST_FAIL();

	if (hog.sched_pri != sched_mlfq_level_priority(SCHED_MLFQ_TOP_LEVEL)) SCHED_SELF_TEST_FAIL();

	/*
	 * Rule 4: a CPU-bound thread that keeps exhausting its quantum drops
	 * one level per exhaustion until it reaches the bottom queue, and
	 * never falls off the bottom.
	 */
	for (uint8_t level = SCHED_MLFQ_TOP_LEVEL; level < SCHED_MLFQ_BOTTOM_LEVEL; level++) {
		if (hog.mlfq_level != level) SCHED_SELF_TEST_FAIL();

		if (hog.quantum_remaining != sched_mlfq_level_quantum(level)) SCHED_SELF_TEST_FAIL();

		sched_mlfq_assign(&hog, (uint8_t)(hog.mlfq_level + 1U));
	}
	if (hog.mlfq_level != SCHED_MLFQ_BOTTOM_LEVEL) SCHED_SELF_TEST_FAIL();

	sched_mlfq_assign(&hog, (uint8_t)(hog.mlfq_level + 1U));
	if (hog.mlfq_level != SCHED_MLFQ_BOTTOM_LEVEL) SCHED_SELF_TEST_FAIL();

	/*
	 * A thread that yields every tick never runs out its quantum (each
	 * dispatch refills it) but still spends its level's allotment: it sinks
	 * one level per quantum's worth of ticks, and takes no earlier than that.
	 */
	sched_mlfq_test_thread(&hog, SCHED_MLFQ_TOP_LEVEL);

	for (uint8_t level = SCHED_MLFQ_TOP_LEVEL; level < SCHED_MLFQ_BOTTOM_LEVEL; level++) {
		uint32_t ticks = sched_mlfq_level_quantum(level);

		for (uint32_t tick = 1U; tick <= ticks; tick++) {
			if (hog.mlfq_level != level) SCHED_SELF_TEST_FAIL();

			bool demoted = sched_mlfq_charge_tick(&hog, 0);

			if (demoted != (tick == ticks)) SCHED_SELF_TEST_FAIL();

			/* The yield: the next dispatch grants a whole quantum again. */
			if (!demoted) hog.quantum_remaining = sched_mlfq_level_quantum(level);
		}
		if (hog.mlfq_level != level + 1U) SCHED_SELF_TEST_FAIL();

		if (hog.mlfq_ticks != 0U) SCHED_SELF_TEST_FAIL();
	}

	/* The bottom level has nowhere lower to go. */
	for (uint32_t tick = 0U; tick < sched_mlfq_level_quantum(SCHED_MLFQ_BOTTOM_LEVEL) * 2U; tick++) {
		(void)sched_mlfq_charge_tick(&hog, 0);
		if (hog.mlfq_level != SCHED_MLFQ_BOTTOM_LEVEL) SCHED_SELF_TEST_FAIL();
	}

	/* Sleeping clears the allotment, so a thread that blocks between short bursts stays where it is. */
	sched_mlfq_test_thread(&hog, SCHED_MLFQ_TOP_LEVEL);

	for (uint32_t round = 0U; round < 8U; round++) {
		for (uint32_t tick = 1U; tick < sched_mlfq_level_quantum(SCHED_MLFQ_TOP_LEVEL); tick++) {
			if (sched_mlfq_charge_tick(&hog, 0)) SCHED_SELF_TEST_FAIL();

			hog.quantum_remaining = sched_mlfq_level_quantum(SCHED_MLFQ_TOP_LEVEL);
		}

		sched_mlfq_blocked(&hog);
	}
	if (hog.mlfq_level != SCHED_MLFQ_TOP_LEVEL) SCHED_SELF_TEST_FAIL();

	/*
	 * Rule 5: threads parked below the top queue, and the currently
	 * running thread, all return to level 0 on a boost.
	 */
	sched_mlfq_test_thread(&queued_mid, SCHED_MLFQ_BOTTOM_LEVEL - 1U);
	sched_mlfq_test_thread(&queued_bottom, SCHED_MLFQ_BOTTOM_LEVEL);
	sched_mlfq_test_thread(&running, SCHED_MLFQ_BOTTOM_LEVEL);

	if (!run_queue_enqueue(&test_processor.runq, &queued_mid, RUN_QUEUE_TAIL)) SCHED_SELF_TEST_FAIL();

	if (!run_queue_enqueue(&test_processor.runq, &queued_bottom, RUN_QUEUE_TAIL)) SCHED_SELF_TEST_FAIL();

	test_processor.active_thread = &running;

	sched_mlfq_boost_locked(&test_processor);

	if (queued_mid.mlfq_level != SCHED_MLFQ_TOP_LEVEL) SCHED_SELF_TEST_FAIL();

	if (queued_bottom.mlfq_level != SCHED_MLFQ_TOP_LEVEL) SCHED_SELF_TEST_FAIL();

	if (running.mlfq_level != SCHED_MLFQ_TOP_LEVEL) SCHED_SELF_TEST_FAIL();

	if (queued_mid.sched_pri != sched_mlfq_level_priority(SCHED_MLFQ_TOP_LEVEL)) SCHED_SELF_TEST_FAIL();

	if (queued_mid.runq != &test_processor.runq) SCHED_SELF_TEST_FAIL();

	if (queued_bottom.runq != &test_processor.runq) SCHED_SELF_TEST_FAIL();

	if (!run_queue_validate(&test_processor.runq)) SCHED_SELF_TEST_FAIL();

	return sched_qos_self_test();
}

/*
 * sched_qos_self_test
 *
 * The QoS bands: strictly ordered with no overlap, a CPU-bound thread stays
 * inside its own, a boost takes it back to its own band's top, and a thread
 * left waiting is lifted by the starvation guard and loses the lift when
 * queued again.
 */
static bool sched_qos_self_test_body(void);

/* The mechanism is tested whether or not sched.qos turned the bands off. */
static bool
sched_qos_self_test(void)
{
	bool enabled = g_sched_qos_enabled;

	g_sched_qos_enabled = true;
	bool passed = sched_qos_self_test_body();
	g_sched_qos_enabled = enabled;
	return passed;
}

static bool
sched_qos_self_test_body(void)
{
	/* Every level of a band is above every level of the bands below it. */
	static const uint8_t order[NXU_QOS_COUNT] = {NXU_QOS_USER_INTERACTIVE, NXU_QOS_USER_INITIATED, NXU_QOS_DEFAULT,
												 NXU_QOS_UTILITY, NXU_QOS_BACKGROUND};
	for (uint32_t index = 0U; index + 1U < NXU_QOS_COUNT; index++) {
		uint16_t floor = sched_qos_priority(order[index], SCHED_MLFQ_BOTTOM_LEVEL);
		uint16_t below = sched_qos_priority(order[index + 1U], SCHED_MLFQ_TOP_LEVEL);

		if (floor <= below) SCHED_SELF_TEST_FAIL();
	}

	static struct task background_task;
	static struct task interactive_task;
	struct thread background;
	struct thread interactive;
	struct processor test_processor;

	memset(&background_task, 0, sizeof(background_task));
	memset(&interactive_task, 0, sizeof(interactive_task));
	background_task.qos = NXU_QOS_BACKGROUND;
	interactive_task.qos = NXU_QOS_USER_INTERACTIVE;

	sched_mlfq_test_thread(&background, SCHED_MLFQ_TOP_LEVEL);
	sched_mlfq_test_thread(&interactive, SCHED_MLFQ_TOP_LEVEL);
	background.task = &background_task;
	interactive.task = &interactive_task;
	sched_mlfq_assign(&background, SCHED_MLFQ_TOP_LEVEL);
	sched_mlfq_assign(&interactive, SCHED_MLFQ_TOP_LEVEL);

	/* A fresh background thread is still below an interactive one that has
	 * sunk all the way down its band. */
	sched_mlfq_assign(&interactive, SCHED_MLFQ_BOTTOM_LEVEL);
	if (background.sched_pri >= interactive.sched_pri) SCHED_SELF_TEST_FAIL();

	/* Starvation: queued long enough ago, the background thread is lifted
	 * above everything; queued just now, it is left alone. */
	memset(&test_processor, 0, sizeof(test_processor));
	run_queue_init(&test_processor.runq);

	if (!run_queue_enqueue(&test_processor.runq, &background, RUN_QUEUE_TAIL)) SCHED_SELF_TEST_FAIL();

	uint32_t queued_at = background.runq_enqueued_tick;

	if (sched_starvation_scan(&test_processor.runq, queued_at + 1U) != 0U) SCHED_SELF_TEST_FAIL();
	if (background.qos_promoted) SCHED_SELF_TEST_FAIL();

	/* A scan that leaves a thread where it was does not restart its wait. */
	if (background.runq_enqueued_tick != queued_at) SCHED_SELF_TEST_FAIL();

	if (sched_starvation_scan(&test_processor.runq, queued_at + SCHED_STARVATION_TICKS) != 1U) SCHED_SELF_TEST_FAIL();
	if (!background.qos_promoted || background.sched_pri != SCHED_STARVATION_PRIORITY) SCHED_SELF_TEST_FAIL();
	if (run_queue_peek(&test_processor.runq) != &background) SCHED_SELF_TEST_FAIL();

	/* Donation: a background task that an interactive one sent a message
	 * to runs interactive until it is done, then at its own class again. */
	sched_task_qos_donate(&background_task, NXU_QOS_USER_INTERACTIVE);
	if (sched_task_effective_qos(&background_task) != NXU_QOS_USER_INTERACTIVE) SCHED_SELF_TEST_FAIL();
	sched_task_qos_donate(&background_task, NXU_QOS_UTILITY);
	if (sched_task_effective_qos(&background_task) != NXU_QOS_USER_INTERACTIVE) SCHED_SELF_TEST_FAIL();
	sched_task_qos_handling(&background_task, (uint8_t)NXU_QOS_COUNT);
	if (sched_task_effective_qos(&background_task) != NXU_QOS_BACKGROUND) SCHED_SELF_TEST_FAIL();
	/* Never lowers: an interactive task handling a utility request stays. */
	sched_task_qos_handling(&interactive_task, NXU_QOS_UTILITY);
	if (sched_task_effective_qos(&interactive_task) != NXU_QOS_USER_INTERACTIVE) SCHED_SELF_TEST_FAIL();

	/* The next level change gives it its own band back. */
	(void)run_queue_remove(&test_processor.runq, &background);
	sched_mlfq_assign(&background, SCHED_MLFQ_TOP_LEVEL);
	if (background.qos_promoted || background.sched_pri != sched_qos_priority(NXU_QOS_BACKGROUND, 0U)) SCHED_SELF_TEST_FAIL();

	return run_queue_validate(&test_processor.runq);
}

bool
sched_validate(void)
{
	if (!g_sched.initialized) return false;

	processor_t processor = current_processor();
	thread_t current = current_thread();

	if (processor == 0 || current == 0 || processor->active_thread != current || processor->idle_thread == 0 ||
		!thread_is_bootstrap(g_sched.bootstrap_thread) || !thread_is_idle(processor->idle_thread) ||
		processor->idle_thread->runq != 0 || processor->previous_thread != 0 || !processor_validate(processor)) {
		return false;
	}

	return true;
}

/*
 * sched_validate_all
 *
 * The cross-CPU invariants, checked with every CPU's run queue locked in
 * ascending id order: each queued thread is on exactly the queue its runq
 * pointer names, is runnable, is not running anywhere, and appears on no other
 * queue. (Whether it may run on the CPU that holds it is not checked: a thread
 * whose affinity was narrowed while it was queued stays queued until it next
 * runs or is balanced away.) Costly (it walks every queue), so only tests call
 * it.
 */
bool
sched_validate_all(void)
{
	if (!g_sched.initialized) return false;

	uint64_t irq_state = ml_irq_save();
	uint32_t count = processor_count();
	bool ok = true;

	for (uint32_t cpu = 0U; cpu < count; cpu++)
		nxu_spin_lock(&processor_by_id(cpu)->runq_lock);

	for (uint32_t cpu = 0U; cpu < count && ok; cpu++) {
		processor_t processor = processor_by_id(cpu);

		if (!run_queue_validate(&processor->runq)) ok = false;

		for (uint32_t priority = 0U; priority < RUN_QUEUE_COUNT && ok; priority++) {
			for (thread_t thread = processor->runq.queues[priority].head; thread != 0 && ok;
				 thread = thread->sched_links.runq.next) {
				if (thread->runq != &processor->runq || thread_is_idle(thread) || !thread_is_runnable(thread) ||
					thread->on_cpu != 0) {
					ok = false;
				}

				/* On no other queue: a thread queued twice would have two runq owners. */
				for (uint32_t other = cpu + 1U; other < count && ok; other++) {
					for (uint32_t level = 0U; level < RUN_QUEUE_COUNT && ok; level++) {
						for (thread_t peer = processor_by_id(other)->runq.queues[level].head; peer != 0;
							 peer = peer->sched_links.runq.next) {
							if (peer == thread) ok = false;
						}
					}
				}
			}
		}
	}
	for (uint32_t cpu = count; cpu > 0U; cpu--)
		nxu_spin_unlock(&processor_by_id(cpu - 1U)->runq_lock);

	ml_irq_restore(irq_state);
	return ok;
}

void
sched_dump(void)
{
	if (!g_sched.initialized) {
		kprintf("not initialized\n");
		return;
	}

	processor_t processor = current_processor();

	kprintf("cpu: %llu\n", (unsigned long long)processor->cpu_id);
	kprintf("current tid: %llu\n", (unsigned long long)thread_tid(processor->active_thread));
	kprintf("idle tid: %llu\n", (unsigned long long)thread_tid(processor->idle_thread));
	kprintf("run queue count: %llu\n", (unsigned long long)processor->runq.count);
	kprintf("context switches: %llu\n", (unsigned long long)processor->context_switch_count);

	if (!kconsole_verbose()) return;

	if (processor->runq.highq == RUN_QUEUE_NONE) {
		kverbosef("run queue highq: none\n");
	} else {
		kverbosef("run queue highq: %llu\n", (unsigned long long)processor->runq.highq);
	}

	kverbosef("dispatches: %llu\n", (unsigned long long)processor->dispatch_count);
	kverbosef("preemptions: %llu\n", (unsigned long long)processor->preemption_count);
	kverbosef("quantum expirations: %llu\n", (unsigned long long)processor->quantum_expiration_count);
	kverbosef("default quantum: %llu tick(s)\n", (unsigned long long)SCHED_DEFAULT_QUANTUM_TICKS);
	kverbosef("MLFQ levels: %llu\n", (unsigned long long)SCHED_MLFQ_LEVELS);
	kverbosef("MLFQ boost interval: %llu tick(s)\n", (unsigned long long)SCHED_MLFQ_BOOST_INTERVAL_TICKS);
	kverbosef("MLFQ next boost in: %llu tick(s)\n",
			  (unsigned long long)(SCHED_MLFQ_BOOST_INTERVAL_TICKS - processor->boost_ticks));

	if (processor->active_thread != 0 && !thread_is_idle(processor->active_thread)) {
		kverbosef("active thread MLFQ level: %llu\n",
				  (unsigned long long)processor->active_thread->mlfq_level);
		kverbosef("active thread quantum remaining: %llu\n",
				  (unsigned long long)processor->active_thread->quantum_remaining);
	}
}
