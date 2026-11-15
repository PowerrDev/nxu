/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/clock/timer_call.c
 *
 * See kern/clock/timer_call.h.
 *
 * Each queue is a doubly-linked list sorted by deadline (equal deadlines in
 * the order they were entered), under one spinlock taken with interrupts
 * masked. Insertion walks from the tail, since a new deadline is most often
 * the latest; a CPU has a handful of pending calls (a timed wait per sleeping
 * thread), so the list stays short.
 *
 * Locking. A call's `queue` field says which queue's lock protects it. To act
 * on a call, read the field, take that queue's lock, and check the field
 * still names it (timer_call_lock_queue); if it changed the call moved, so
 * try again. A call moves only while its old queue's lock is held, by storing
 * the migrating marker first: anyone who then looks waits for the move to
 * finish instead of acting on either queue. No code holds two queue locks at
 * once, and a queue lock is a leaf: callbacks run with it released, and the
 * event timer is reprogrammed after it is released.
 *
 * Running callbacks are counted in the call itself (`running`), so a call can
 * be re-entered on another CPU while its callback still runs here, and
 * timer_call_cancel_sync still knows to wait for it.
 */

#include <kern/clock/timer_call.h>

#include <kern/clock/etimer.h>
#include <kern/console/console.h>
#include <kern/cpuset.h>
#include <kern/machine/cpu.h>
#include <kern/machine/machine_routines.h>
#include <kern/machine/smp.h>

#include <stdbool.h>
#include <stdint.h>

struct timer_call_queue {
	nxu_spinlock_t lock;
	timer_call_t head;
	timer_call_t tail;

	/* The CPU whose event timer runs this queue, or TIMER_CALL_QUEUE_PRIVATE (self-test queues). */
	uint32_t cpu;

	/* The call whose callback this queue's CPU is running right now (cancel_sync's self-check). */
	timer_call_t running_call;

	timer_call_stats_t stats;
};

#define TIMER_CALL_QUEUE_PRIVATE UINT32_MAX

/* Stored in call->queue while a call moves between queues; never dereferenced. */
#define TIMER_CALL_MIGRATING ((struct timer_call_queue *)1)

static struct timer_call_queue g_timer_call_queues[NXU_MAX_CPUS];
static bool g_timer_call_initialized;

static void
timer_call_queue_init(struct timer_call_queue *queue, uint32_t cpu)
{
	queue->lock.value = 0U;
	queue->head = 0;
	queue->tail = 0;
	queue->cpu = cpu;
	queue->running_call = 0;
	queue->stats = (timer_call_stats_t){0};
}

void
timer_call_init(void)
{
	kprintf("initializing timer call subsystem\n");

	for (uint32_t cpu = 0U; cpu < NXU_MAX_CPUS; cpu++)
		timer_call_queue_init(&g_timer_call_queues[cpu], cpu);

	__atomic_store_n(&g_timer_call_initialized, true, __ATOMIC_RELEASE);

	kprintf("%u timer queues initialized, one per possible CPU\n", (uint32_t)NXU_MAX_CPUS);
}

/*
 * The queue a call entered now goes on: this CPU's when the event timer is
 * per-CPU, the boot CPU's when a periodic tick on the boot CPU is all there
 * is (every architecture before its event timer starts, and i386 without a
 * usable Local APIC timer).
 */
static struct timer_call_queue *
timer_call_local_queue(void)
{
	uint32_t cpu = etimer_deadline_mode() ? machine_cpu_id() : 0U;

	if (cpu >= NXU_MAX_CPUS) cpu = 0U;

	return &g_timer_call_queues[cpu];
}

/* Lock the queue `call` belongs to and return it, or return 0 (nothing locked) for a never-entered call. */
static struct timer_call_queue *
timer_call_lock_queue(timer_call_t call)
{
	for (;;) {
		struct timer_call_queue *queue = __atomic_load_n(&call->queue, __ATOMIC_ACQUIRE);

		if (queue == 0) return 0;

		if (queue == TIMER_CALL_MIGRATING) {
			cpu_relax();
			continue;
		}

		nxu_spin_lock(&queue->lock);

		if (__atomic_load_n(&call->queue, __ATOMIC_ACQUIRE) == queue) return queue;

		nxu_spin_unlock(&queue->lock);
	}
}

static void
timer_call_unlink_locked(struct timer_call_queue *queue, timer_call_t call)
{
	if (call->prev != 0) {
		call->prev->next = call->next;
	} else {
		queue->head = call->next;
	}

	if (call->next != 0) {
		call->next->prev = call->prev;
	} else {
		queue->tail = call->prev;
	}

	call->next = 0;
	call->prev = 0;
	call->pending = false;
	queue->stats.pending--;
}

/* Insert in deadline order, after any call with the same deadline. */
static void
timer_call_insert_locked(struct timer_call_queue *queue, timer_call_t call)
{
	timer_call_t after = queue->tail;

	while (after != 0 && after->deadline > call->deadline)
		after = after->prev;

	call->prev = after;
	call->next = after != 0 ? after->next : queue->head;

	if (call->next != 0) {
		call->next->prev = call;
	} else {
		queue->tail = call;
	}

	if (after != 0) {
		after->next = call;
	} else {
		queue->head = call;
	}

	call->pending = true;
	queue->stats.pending++;
	queue->stats.entered++;
}

void
timer_call_setup(timer_call_t call, timer_call_func_t func, void *param0, void *param1)
{
	if (call == 0) return;

	call->func = func;
	call->param0 = param0;
	call->param1 = param1;
}

/*
 * Lock `target` with `call` belonging to it, taking the call off whatever
 * queue it was pending on. Returns whether it was pending. Interrupts masked.
 */
static bool
timer_call_claim(timer_call_t call, struct timer_call_queue *target)
{
	for (;;) {
		struct timer_call_queue *queue = timer_call_lock_queue(call);

		if (queue == 0) {
			/* Never entered: claim it for `target`, unless another CPU does first. */
			nxu_spin_lock(&target->lock);

			struct timer_call_queue *expected = 0;

			if (__atomic_compare_exchange_n(&call->queue, &expected, target, false, __ATOMIC_ACQ_REL,
											__ATOMIC_ACQUIRE)) {
				return false;
			}

			nxu_spin_unlock(&target->lock);
			continue;
		}

		bool was_pending = call->pending;

		if (was_pending) timer_call_unlink_locked(queue, call);

		if (queue == target) return was_pending;

		/* Move it: nobody acts on the call while it says "migrating", so no lock covers the gap. */
		__atomic_store_n(&call->queue, TIMER_CALL_MIGRATING, __ATOMIC_RELEASE);
		nxu_spin_unlock(&queue->lock);

		nxu_spin_lock(&target->lock);
		__atomic_store_n(&call->queue, target, __ATOMIC_RELEASE);
		return was_pending;
	}
}

/* Enter `call` on `target`; returns whether it was pending. Sets *new_head when it became the earliest there. */
static bool
timer_call_enter_on(timer_call_t call, uint64_t deadline, struct timer_call_queue *target, bool *new_head)
{
	uint64_t irq_state = ml_irq_save();
	bool was_pending = timer_call_claim(call, target);

	call->deadline = deadline;
	timer_call_insert_locked(target, call);
	*new_head = target->head == call;

	nxu_spin_unlock(&target->lock);
	ml_irq_restore(irq_state);
	return was_pending;
}

bool
timer_call_enter(timer_call_t call, uint64_t deadline)
{
	if (call == 0 || call->func == 0) return false;

	if (!__atomic_load_n(&g_timer_call_initialized, __ATOMIC_ACQUIRE)) {
		kprintf("timer call entered before timer_call_init\n");
		return false;
	}

	/*
	 * Interrupts stay masked from choosing the queue to reprogramming the
	 * timer, so the thread cannot move to another CPU in between and
	 * reprogram the wrong one.
	 */
	uint64_t irq_state = ml_irq_save();
	struct timer_call_queue *target = timer_call_local_queue();
	bool new_head = false;
	bool was_pending = timer_call_enter_on(call, deadline, target, &new_head);

	if (new_head) etimer_resync();

	ml_irq_restore(irq_state);
	return was_pending;
}

bool
timer_call_cancel(timer_call_t call)
{
	if (call == 0) return false;

	uint64_t irq_state = ml_irq_save();
	struct timer_call_queue *queue = timer_call_lock_queue(call);
	bool was_pending = false;

	if (queue != 0) {
		was_pending = call->pending;

		if (was_pending) {
			timer_call_unlink_locked(queue, call);
			queue->stats.cancelled++;
		}

		nxu_spin_unlock(&queue->lock);
	}

	/*
	 * The hardware may stay armed for a deadline that is gone: it fires
	 * early, finds nothing due and is reprogrammed, which costs less than
	 * reprogramming on every cancel.
	 */
	ml_irq_restore(irq_state);
	return was_pending;
}

bool
timer_call_cancel_sync(timer_call_t call)
{
	if (call == 0) return false;

	bool was_pending = timer_call_cancel(call);

	if (__atomic_load_n(&call->running, __ATOMIC_ACQUIRE) == 0U) return was_pending;

	/* Running here can only mean the caller is that callback: waiting would never end. */
	uint32_t self = machine_cpu_id();

	if (self < NXU_MAX_CPUS && __atomic_load_n(&g_timer_call_queues[self].running_call, __ATOMIC_ACQUIRE) == call) {
		kprintf("called from the call's own callback, not waiting for it\n");
		return was_pending;
	}

	while (__atomic_load_n(&call->running, __ATOMIC_ACQUIRE) != 0U)
		cpu_relax();

	/* A callback that re-entered its call before returning left it pending again: that entry is cancelled too. */
	return timer_call_cancel(call) || was_pending;
}

bool
timer_call_is_pending(timer_call_t call)
{
	return call != 0 && __atomic_load_n(&call->pending, __ATOMIC_ACQUIRE);
}

static uint64_t
timer_call_queue_next(struct timer_call_queue *queue)
{
	uint64_t irq_state = nxu_spin_lock_irqsave(&queue->lock);
	uint64_t deadline = queue->head != 0 ? queue->head->deadline : TIMER_CALL_NO_DEADLINE;

	nxu_spin_unlock_irqrestore(&queue->lock, irq_state);
	return deadline;
}

uint64_t
timer_call_next_deadline(uint32_t cpu)
{
	if (cpu >= NXU_MAX_CPUS) return TIMER_CALL_NO_DEADLINE;

	return timer_call_queue_next(&g_timer_call_queues[cpu]);
}

static uint32_t
timer_call_queue_expire(struct timer_call_queue *queue, uint64_t now)
{
	uint32_t count = 0U;
	uint64_t irq_state = nxu_spin_lock_irqsave(&queue->lock);

	for (;;) {
		timer_call_t call = queue->head;

		if (call == 0 || call->deadline > now) break;

		timer_call_unlink_locked(queue, call);
		queue->stats.expired++;

		uint64_t lateness = now - call->deadline;

		if (lateness > queue->stats.max_lateness_us) queue->stats.max_lateness_us = lateness;

		timer_call_func_t func = call->func;
		void *param0 = call->param0;
		void *param1 = call->param1;

		(void)__atomic_add_fetch(&call->running, 1U, __ATOMIC_ACQ_REL);
		__atomic_store_n(&queue->running_call, call, __ATOMIC_RELEASE);
		nxu_spin_unlock(&queue->lock);

		func(param0, param1);
		count++;

		nxu_spin_lock(&queue->lock);
		__atomic_store_n(&queue->running_call, (timer_call_t)0, __ATOMIC_RELEASE);

		/* Last touch of the call: from here its owner may free it (cancel_sync). */
		(void)__atomic_sub_fetch(&call->running, 1U, __ATOMIC_ACQ_REL);
	}

	nxu_spin_unlock_irqrestore(&queue->lock, irq_state);
	return count;
}

uint32_t
timer_call_expire(uint32_t cpu, uint64_t now)
{
	if (cpu >= NXU_MAX_CPUS || !__atomic_load_n(&g_timer_call_initialized, __ATOMIC_ACQUIRE)) return 0U;

	return timer_call_queue_expire(&g_timer_call_queues[cpu], now);
}

static uint32_t
timer_call_queue_move_all(struct timer_call_queue *from, struct timer_call_queue *to)
{
	uint32_t moved = 0U;

	if (from == to) return 0U;

	for (;;) {
		uint64_t irq_state = nxu_spin_lock_irqsave(&from->lock);
		timer_call_t call = from->head;

		if (call == 0) {
			nxu_spin_unlock_irqrestore(&from->lock, irq_state);
			return moved;
		}

		/* As timer_call_claim: unlink, mark migrating, and finish the move under the new queue's lock. */
		timer_call_unlink_locked(from, call);
		__atomic_store_n(&call->queue, TIMER_CALL_MIGRATING, __ATOMIC_RELEASE);
		nxu_spin_unlock(&from->lock);

		nxu_spin_lock(&to->lock);
		__atomic_store_n(&call->queue, to, __ATOMIC_RELEASE);
		timer_call_insert_locked(to, call);
		to->stats.entered--; /* moved, not entered */
		nxu_spin_unlock_irqrestore(&to->lock, irq_state);
		moved++;
	}
}

uint32_t
timer_call_queue_migrate(uint32_t from, uint32_t to)
{
	if (from >= NXU_MAX_CPUS || to >= NXU_MAX_CPUS) return 0U;

	uint32_t moved = timer_call_queue_move_all(&g_timer_call_queues[from], &g_timer_call_queues[to]);

	if (moved != 0U) {
		kprintf("moved %u timer call(s) from CPU %u to CPU %u\n", moved, from, to);
		etimer_resync();
	}

	return moved;
}

void
timer_call_stats(uint32_t cpu, timer_call_stats_t *out)
{
	if (out == 0) return;

	if (cpu >= NXU_MAX_CPUS) {
		*out = (timer_call_stats_t){0};
		return;
	}

	struct timer_call_queue *queue = &g_timer_call_queues[cpu];
	uint64_t irq_state = nxu_spin_lock_irqsave(&queue->lock);

	*out = queue->stats;
	nxu_spin_unlock_irqrestore(&queue->lock, irq_state);
}

/* ---- self-test ------------------------------------------------------------ */

#define TIMER_CALL_TEST_CALLS 6U

static uint32_t g_timer_call_test_order[TIMER_CALL_TEST_CALLS * 2U];
static uint32_t g_timer_call_test_ran;

static void
timer_call_test_record(void *param0, void *param1)
{
	(void)param1;

	if (g_timer_call_test_ran < sizeof(g_timer_call_test_order) / sizeof(g_timer_call_test_order[0]))
		g_timer_call_test_order[g_timer_call_test_ran] = (uint32_t)(uintptr_t)param0;

	g_timer_call_test_ran++;
}

/* A callback that enters its own call again, 10 us later, until it has run three times. */
static struct timer_call_queue *g_timer_call_test_rearm_queue;

static void
timer_call_test_rearm(void *param0, void *param1)
{
	timer_call_t call = (timer_call_t)param0;
	bool new_head;

	(void)param1;
	g_timer_call_test_ran++;

	if (g_timer_call_test_ran < 3U) (void)timer_call_enter_on(call, call->deadline + 10ULL, g_timer_call_test_rearm_queue, &new_head);
}

#define TIMER_CALL_CHECK(condition, what)                       \
	do {                                                        \
		if (!(condition)) {                                     \
			kprintf("FAIL %s\n", what);                         \
			return false;                                       \
		}                                                       \
	} while (0)

bool
timer_call_self_test(void)
{
	static struct timer_call_queue queue;
	static struct timer_call_queue other;
	static struct timer_call calls[TIMER_CALL_TEST_CALLS];
	bool new_head;

	timer_call_queue_init(&queue, TIMER_CALL_QUEUE_PRIVATE);
	timer_call_queue_init(&other, TIMER_CALL_QUEUE_PRIVATE);

	for (uint32_t index = 0U; index < TIMER_CALL_TEST_CALLS; index++) {
		calls[index] = (struct timer_call){0};
		timer_call_setup(&calls[index], timer_call_test_record, (void *)(uintptr_t)index, 0);
	}

	/* Entered out of order, with two equal deadlines: run in deadline order, equal ones first-come. */
	static const uint64_t deadlines[TIMER_CALL_TEST_CALLS] = {500ULL, 100ULL, 300ULL, 300ULL, 900ULL, 200ULL};

	for (uint32_t index = 0U; index < TIMER_CALL_TEST_CALLS; index++)
		(void)timer_call_enter_on(&calls[index], deadlines[index], &queue, &new_head);

	TIMER_CALL_CHECK(queue.stats.pending == TIMER_CALL_TEST_CALLS, "every entered call is pending");
	TIMER_CALL_CHECK(timer_call_queue_next(&queue) == 100ULL, "the earliest deadline heads the queue");

	g_timer_call_test_ran = 0U;
	TIMER_CALL_CHECK(timer_call_queue_expire(&queue, 99ULL) == 0U, "nothing runs before its deadline");
	TIMER_CALL_CHECK(timer_call_queue_expire(&queue, 300ULL) == 4U, "everything due runs at once");
	TIMER_CALL_CHECK(g_timer_call_test_order[0] == 1U && g_timer_call_test_order[1] == 5U &&
						 g_timer_call_test_order[2] == 2U && g_timer_call_test_order[3] == 3U,
					 "due calls run in deadline order, equal deadlines first-come");
	TIMER_CALL_CHECK(queue.stats.max_lateness_us == 200ULL, "lateness is measured from the deadline");

	/* Rescheduling a pending call moves it; cancelling one keeps it from running. */
	TIMER_CALL_CHECK(timer_call_enter_on(&calls[4], 400ULL, &queue, &new_head), "re-entering a pending call reports it was pending");
	TIMER_CALL_CHECK(new_head, "an earlier deadline becomes the head");
	TIMER_CALL_CHECK(timer_call_cancel(&calls[0]), "cancelling a pending call reports it was pending");
	TIMER_CALL_CHECK(!timer_call_cancel(&calls[0]), "cancelling it again finds it no longer pending");
	TIMER_CALL_CHECK(!timer_call_cancel_sync(&calls[1]), "a call that already ran is not pending");

	g_timer_call_test_ran = 0U;
	TIMER_CALL_CHECK(timer_call_queue_expire(&queue, 10000ULL) == 1U && g_timer_call_test_order[0] == 4U,
					 "only the rescheduled call is left, and it runs");
	TIMER_CALL_CHECK(queue.head == 0 && queue.tail == 0 && queue.stats.pending == 0U, "the queue ends empty");

	/* A call moves between queues whole: pending on one at a time, deadline kept. */
	(void)timer_call_enter_on(&calls[2], 700ULL, &queue, &new_head);
	(void)timer_call_enter_on(&calls[3], 600ULL, &queue, &new_head);
	(void)timer_call_enter_on(&calls[2], 650ULL, &other, &new_head);
	TIMER_CALL_CHECK(calls[2].queue == &other && queue.stats.pending == 1U && other.stats.pending == 1U,
					 "entering on another queue moves the call");
	TIMER_CALL_CHECK(timer_call_queue_move_all(&queue, &other) == 1U, "migrating a queue moves its calls");
	TIMER_CALL_CHECK(timer_call_queue_next(&other) == 600ULL && calls[3].queue == &other && queue.head == 0,
					 "migrated calls keep their deadlines and order");

	g_timer_call_test_ran = 0U;
	TIMER_CALL_CHECK(timer_call_queue_expire(&other, 650ULL) == 2U && g_timer_call_test_order[0] == 3U &&
						 g_timer_call_test_order[1] == 2U,
					 "migrated calls run in order on their new queue");

	/* A callback may enter its own call again. */
	timer_call_t rearm = &calls[5];

	timer_call_setup(rearm, timer_call_test_rearm, rearm, 0);
	g_timer_call_test_rearm_queue = &queue;
	g_timer_call_test_ran = 0U;
	(void)timer_call_enter_on(rearm, 1000ULL, &queue, &new_head);
	TIMER_CALL_CHECK(timer_call_queue_expire(&queue, 1000ULL) == 1U && timer_call_is_pending(rearm),
					 "a callback re-entering its call leaves it pending for the next deadline");
	TIMER_CALL_CHECK(timer_call_queue_expire(&queue, 1100ULL) == 2U && !timer_call_is_pending(rearm) &&
						 rearm->running == 0U,
					 "a re-entered call runs again at its new deadline");

	return true;
}
