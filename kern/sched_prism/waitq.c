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
 */

#include <kern/sched_prism/waitq.h>

#include <kern/machine/machine_routines.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>

void waitq_init(waitq_t *queue)
{
	if (queue == 0) return;

	queue->head = 0;
	queue->tail = 0;
	queue->lock.value = 0U;
	queue->seq = 0U;
}

static void waitq_enqueue_locked(waitq_t *queue, thread_t thread, bool interruptible)
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

void waitq_enqueue(waitq_t *queue, thread_t thread, bool interruptible)
{
	if (queue == 0 || thread == 0) return;

	uint64_t irq_state = nxu_spin_lock_irqsave(&queue->lock);

	waitq_enqueue_locked(queue, thread, interruptible);

	nxu_spin_unlock_irqrestore(&queue->lock, irq_state);
}

thread_t waitq_dequeue(waitq_t *queue)
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

bool waitq_remove(thread_t thread)
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

		nxu_spin_unlock_irqrestore(&queue->lock, irq_state);
		return true;
	}
}

uint32_t waitq_seq(waitq_t *queue)
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
void waitq_wake_one(waitq_t *queue)
{
	if (queue == 0) return;

	(void)__atomic_add_fetch(&queue->seq, 1U, __ATOMIC_ACQ_REL);

	thread_t thread = waitq_dequeue(queue);

	if (thread != 0) (void)sched_thread_wakeup(thread);
}

void waitq_wake_all(waitq_t *queue)
{
	thread_t thread;

	if (queue == 0) return;

	(void)__atomic_add_fetch(&queue->seq, 1U, __ATOMIC_ACQ_REL);

	while ((thread = waitq_dequeue(queue)) != 0) {
		(void)sched_thread_wakeup(thread);
	}
}

bool waitq_interrupt(thread_t thread)
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
static bool waitq_sleep(
	waitq_t *queue,
	bool interruptible,
	nxu_spinlock_t *guard,
	uint64_t irq_state,
	const uint32_t *expected_seq
)
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

bool waitq_block(waitq_t *queue, bool interruptible)
{
	if (queue == 0 || current_thread() == 0) return false;

	uint64_t irq_state = ml_irq_save();

	return waitq_sleep(queue, interruptible, 0, irq_state, 0);
}

bool waitq_block_seq(waitq_t *queue, uint32_t seq, bool interruptible)
{
	if (queue == 0 || current_thread() == 0) return false;

	uint64_t irq_state = ml_irq_save();

	return waitq_sleep(queue, interruptible, 0, irq_state, &seq);
}

bool waitq_block_unlock(waitq_t *queue, bool interruptible, nxu_spinlock_t *guard, uint64_t irq_state)
{
	if (queue == 0 || guard == 0 || current_thread() == 0) {
		if (guard != 0) nxu_spin_unlock(guard);
		ml_irq_restore(irq_state);
		return false;
	}

	return waitq_sleep(queue, interruptible, guard, irq_state, 0);
}
