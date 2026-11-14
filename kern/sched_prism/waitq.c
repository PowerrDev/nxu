/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/sched_prism/waitq.c
 *
 * See kern/sched_prism/waitq.h.
 *
 * Locking: queue->lock protects head, tail and the waitq links of the threads
 * on the queue, and thread->wait_queue while the thread is on it. It is a leaf
 * apart from one nesting: waitq_block takes g_thread_lock (through
 * sched_thread_wait) inside it. Wakeups happen after it is released, so it is
 * never held together with a run queue lock.
 *
 * Signals: an interruptible sleep does not start while the sleeper has a
 * signal pending that it would take (waitq_sleep). A signal posted to a thread
 * that is not asleep yet (still in user mode, or on its way to the sleep) is
 * only marked pending; nothing else would end a sleep begun after it. The
 * test is made with the thread already on the queue, so a signal posted after
 * it finds the thread there and interrupts the sleep (signal_post wakes
 * interruptible sleepers once the signal is pending).
 *
 * Timed waits add one lock, g_waitq_timer_lock, which protects the list of
 * threads sleeping with a deadline and their wait_timer_* fields. It is the
 * outermost lock here: the tick holds it while it takes a thread off its queue
 * (queue->lock) and wakes it (the scheduler's locks), and nothing takes it
 * while holding either of those. Holding it across the wakeup is what keeps
 * the thread alive for the tick: a thread cannot be reclaimed until
 * waitq_timer_cancel has been through this lock.
 */

#include <kern/sched_prism/waitq.h>

#include <kern/machine/machine_routines.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/process/signal.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>

void
waitq_init(waitq_t *queue)
{
	if (queue == 0) return;

	queue->head = 0;
	queue->tail = 0;
	queue->lock.value = 0U;
	queue->seq = 0U;
}

static void
waitq_enqueue_locked(waitq_t *queue, thread_t thread, bool interruptible)
{
	thread->sched_links.waitq.next = 0;
	thread->sched_links.waitq.prev = queue->tail;

	if (queue->tail != 0) {
		queue->tail->sched_links.waitq.next = thread;
	} else {
		queue->head = thread;
	}

	queue->tail = thread;

	thread->wait_queue = queue;
	thread->wait_interruptible = interruptible;
}

void
waitq_enqueue(waitq_t *queue, thread_t thread, bool interruptible)
{
	if (queue == 0 || thread == 0) return;

	uint64_t irq_state = nxu_spin_lock_irqsave(&queue->lock);

	waitq_enqueue_locked(queue, thread, interruptible);

	nxu_spin_unlock_irqrestore(&queue->lock, irq_state);
}

thread_t
waitq_dequeue(waitq_t *queue)
{
	if (queue == 0) return 0;

	uint64_t irq_state = nxu_spin_lock_irqsave(&queue->lock);

	thread_t thread = queue->head;

	if (thread != 0) {
		queue->head = thread->sched_links.waitq.next;

		if (queue->head != 0) {
			queue->head->sched_links.waitq.prev = 0;
		} else {
			queue->tail = 0;
		}

		thread->sched_links.waitq.next = 0;
		thread->sched_links.waitq.prev = 0;
		thread->wait_queue = 0;
		thread->wait_interruptible = false;
	}

	nxu_spin_unlock_irqrestore(&queue->lock, irq_state);
	return thread;
}

/* Take `thread` off `queue`, which it is on; queue->lock held. */
static void
waitq_unlink_locked(waitq_t *queue, thread_t thread)
{
	thread_t previous = thread->sched_links.waitq.prev;
	thread_t next = thread->sched_links.waitq.next;

	if (previous != 0) {
		previous->sched_links.waitq.next = next;
	} else {
		queue->head = next;
	}

	if (next != 0) {
		next->sched_links.waitq.prev = previous;
	} else {
		queue->tail = previous;
	}

	thread->sched_links.waitq.next = 0;
	thread->sched_links.waitq.prev = 0;
	thread->wait_queue = 0;
	thread->wait_interruptible = false;
}

bool
waitq_remove(thread_t thread)
{
	if (thread == 0) return false;

	/*
	 * The queue is found from the thread, and a waker on another CPU can take
	 * the thread off it before this one has the lock; the check under the lock
	 * says whether it still is where it was.
	 */
	for (;;) {
		waitq_t *queue = __atomic_load_n(&thread->wait_queue, __ATOMIC_ACQUIRE);

		if (queue == 0) return false;

		uint64_t irq_state = nxu_spin_lock_irqsave(&queue->lock);

		if (thread->wait_queue != queue) {
			nxu_spin_unlock_irqrestore(&queue->lock, irq_state);
			continue;
		}

		waitq_unlink_locked(queue, thread);

		nxu_spin_unlock_irqrestore(&queue->lock, irq_state);
		return true;
	}

}

uint32_t
waitq_seq(waitq_t *queue)
{
	return queue == 0 ? 0U : __atomic_load_n(&queue->seq, __ATOMIC_ACQUIRE);
}

/*
 * The bump comes before the dequeue and needs no lock: a sleeper checks the
 * sequence under the queue's lock and joins the queue in the same critical
 * section, and the dequeue below takes that lock, so a wake either changes the
 * sequence before the sleeper's check (the sleeper does not sleep) or dequeues
 * after the sleeper joined (it finds it).
 */
void
waitq_wake_one(waitq_t *queue)
{
	if (queue == 0) return;

	(void)__atomic_add_fetch(&queue->seq, 1U, __ATOMIC_ACQ_REL);

	thread_t thread = waitq_dequeue(queue);

	if (thread != 0) (void)sched_thread_wakeup(thread);
}

void
waitq_wake_all(waitq_t *queue)
{
	thread_t thread;

	if (queue == 0) return;

	(void)__atomic_add_fetch(&queue->seq, 1U, __ATOMIC_ACQ_REL);

	while ((thread = waitq_dequeue(queue)) != 0) {
		(void)sched_thread_wakeup(thread);
	}
}

bool
waitq_interrupt(thread_t thread)
{
	if (thread == 0 || thread->wait_queue == 0 || !thread->wait_interruptible) return false;

	/*
	 * Only the caller that actually unlinks the thread wakes it: a normal wakeup
	 * that got there first already did, and waking twice would queue it twice.
	 */
	if (!waitq_remove(thread)) return false;

	thread->wait_interrupted = true;

	return sched_thread_wakeup(thread);
}

/*
 * waitq_sleep
 *
 * Join `queue`, mark the calling thread waiting, both under the queue's lock,
 * and switch away. The caller has interrupts masked (irq_state is what to restore)
 * and, when it holds a guard, releases it after the thread is on the queue.
 */
static bool
waitq_sleep(waitq_t *queue, bool interruptible, nxu_spinlock_t *guard, uint64_t irq_state, const uint32_t *expected_seq)
{
	thread_t thread = current_thread();

	if (thread == 0) {
		if (guard != 0) nxu_spin_unlock(guard);
		ml_irq_restore(irq_state);
		return false;
	}

	thread->wait_interrupted = false;

	nxu_spin_lock(&queue->lock);

	if (expected_seq != 0 && __atomic_load_n(&queue->seq, __ATOMIC_ACQUIRE) != *expected_seq) {
		/* A wakeup has happened since the caller looked: do not sleep, look again. */
		nxu_spin_unlock(&queue->lock);

		if (guard != 0) nxu_spin_unlock(guard);

		ml_irq_restore(irq_state);
		return true;
	}

	waitq_enqueue_locked(queue, thread, interruptible);

	/*
	 * A signal already pending would wait behind this sleep: the call returns
	 * "interrupted" instead, and the signal is taken on the way out. On the
	 * queue first, then the test (the fence orders the two against the
	 * poster's "pending, then interrupt the sleepers"); see the file header.
	 */
	if (interruptible) {
		proc_t proc = current_proc();

		__atomic_thread_fence(__ATOMIC_SEQ_CST);

		if (proc != 0 && signal_pending_for(proc, thread)) {
			waitq_unlink_locked(queue, thread);
			nxu_spin_unlock(&queue->lock);

			if (guard != 0) nxu_spin_unlock(guard);

			ml_irq_restore(irq_state);
			return false;
		}
	}

	bool waiting = sched_thread_wait(thread, false);

	nxu_spin_unlock(&queue->lock);

	if (guard != 0) nxu_spin_unlock(guard);

	/*
	 * Interrupts are still masked, and stay so until the switch is done: an
	 * interrupt between marking the thread waiting and switching away must not
	 * switch away for it.
	 */
	bool switched = waiting && sched_block_commit();

	ml_irq_restore(irq_state);

	/* Woken by whoever made the condition true; make sure we are off the
	 * queue if the scheduler resumed us for any other reason. */
	(void)waitq_remove(thread);

	return switched && !thread->wait_interrupted;
}

static nxu_spinlock_t g_waitq_timer_lock = NXU_SPINLOCK_INIT;
static thread_t g_waitq_timer_head;

/* How many threads are on the list; lets the tick skip the lock when there are none. */
static uint32_t g_waitq_timer_count;

static void
waitq_timer_arm_locked(thread_t thread, uint64_t deadline)
{
	thread->wait_deadline = deadline;
	thread->wait_timed_out = false;
	thread->wait_timer_prev = 0;
	thread->wait_timer_next = g_waitq_timer_head;

	if (g_waitq_timer_head != 0) g_waitq_timer_head->wait_timer_prev = thread;

	g_waitq_timer_head = thread;
	thread->wait_timer_armed = true;

	(void)__atomic_add_fetch(&g_waitq_timer_count, 1U, __ATOMIC_ACQ_REL);
}

static void
waitq_timer_disarm_locked(thread_t thread)
{
	if (!thread->wait_timer_armed) return;

	if (thread->wait_timer_prev != 0) {
		thread->wait_timer_prev->wait_timer_next = thread->wait_timer_next;
	} else {
		g_waitq_timer_head = thread->wait_timer_next;
	}

	if (thread->wait_timer_next != 0) thread->wait_timer_next->wait_timer_prev = thread->wait_timer_prev;

	thread->wait_timer_next = 0;
	thread->wait_timer_prev = 0;
	thread->wait_timer_armed = false;

	(void)__atomic_sub_fetch(&g_waitq_timer_count, 1U, __ATOMIC_ACQ_REL);
}

void
waitq_timer_tick(uint64_t now)
{
	if (__atomic_load_n(&g_waitq_timer_count, __ATOMIC_ACQUIRE) == 0U) return;

	uint64_t irq_state = nxu_spin_lock_irqsave(&g_waitq_timer_lock);

	for (thread_t thread = g_waitq_timer_head; thread != 0;) {
		thread_t next = thread->wait_timer_next;

		/*
		 * As for a signal (waitq_interrupt): only whoever unlinks the thread
		 * from its queue wakes it. A thread that is past its deadline but not on
		 * a queue was either woken a moment ago, and will take itself off this
		 * list, or has not gone to sleep yet, and is found again on the next
		 * tick; either way it is left alone now.
		 */
		if (thread->wait_deadline <= now && waitq_remove(thread)) {
			waitq_timer_disarm_locked(thread);
			thread->wait_timed_out = true;

			(void)sched_thread_wakeup(thread);
		}

		thread = next;
	}

	nxu_spin_unlock_irqrestore(&g_waitq_timer_lock, irq_state);
}

void
waitq_timer_cancel(thread_t thread)
{
	if (thread == 0) return;

	/* Always through the lock, armed or not: a tick that already chose this thread finishes with it first. */
	uint64_t irq_state = nxu_spin_lock_irqsave(&g_waitq_timer_lock);

	waitq_timer_disarm_locked(thread);

	nxu_spin_unlock_irqrestore(&g_waitq_timer_lock, irq_state);
}

uint32_t
waitq_timer_waiters(void)
{
	return __atomic_load_n(&g_waitq_timer_count, __ATOMIC_ACQUIRE);
}

waitq_result_t
waitq_block_deadline(waitq_t *queue, uint32_t seq, bool interruptible, uint64_t deadline)
{
	thread_t thread = current_thread();

	if (queue == 0 || thread == 0) return WAITQ_INTERRUPTED;

	if (deadline == WAITQ_NO_DEADLINE) return waitq_block_seq(queue, seq, interruptible) ? WAITQ_WOKEN : WAITQ_INTERRUPTED;

	if (timer_get_microseconds() >= deadline) return WAITQ_TIMED_OUT;

	/*
	 * Before the scheduler ticks nothing would end the wait at its deadline,
	 * so do not sleep: yield until a wakeup moves the sequence or time runs out.
	 */
	if (!sched_is_ticking()) {
		while (timer_get_microseconds() < deadline) {
			if (waitq_seq(queue) != seq) return WAITQ_WOKEN;

			(void)sched_yield();
		}

		return WAITQ_TIMED_OUT;
	}

	uint64_t irq_state = nxu_spin_lock_irqsave(&g_waitq_timer_lock);

	waitq_timer_arm_locked(thread, deadline);

	nxu_spin_unlock_irqrestore(&g_waitq_timer_lock, irq_state);

	/*
	 * The deadline is armed before the thread joins the queue. If it passes in
	 * between, the tick finds the thread on no queue and leaves it armed; the
	 * next tick ends the sleep that starts below.
	 */
	bool woken = waitq_block_seq(queue, seq, interruptible);

	irq_state = nxu_spin_lock_irqsave(&g_waitq_timer_lock);

	waitq_timer_disarm_locked(thread);

	bool timed_out = thread->wait_timed_out;

	thread->wait_timed_out = false;

	nxu_spin_unlock_irqrestore(&g_waitq_timer_lock, irq_state);

	if (timed_out) return WAITQ_TIMED_OUT;

	return woken ? WAITQ_WOKEN : WAITQ_INTERRUPTED;
}

bool
waitq_block(waitq_t *queue, bool interruptible)
{
	if (queue == 0 || current_thread() == 0) return false;

	uint64_t irq_state = ml_irq_save();

	return waitq_sleep(queue, interruptible, 0, irq_state, 0);
}

bool
waitq_block_seq(waitq_t *queue, uint32_t seq, bool interruptible)
{
	if (queue == 0 || current_thread() == 0) return false;

	uint64_t irq_state = ml_irq_save();

	return waitq_sleep(queue, interruptible, 0, irq_state, &seq);
}

bool
waitq_block_unlock(waitq_t *queue, bool interruptible, nxu_spinlock_t *guard, uint64_t irq_state)
{
	if (queue == 0 || guard == 0 || current_thread() == 0) {
		if (guard != 0) nxu_spin_unlock(guard);
		ml_irq_restore(irq_state);
		return false;
	}

	return waitq_sleep(queue, interruptible, guard, irq_state, 0);
}
