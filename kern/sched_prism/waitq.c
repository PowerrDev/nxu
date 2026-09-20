/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/sched_prism/waitq.c
 *
 * See kern/sched_prism/waitq.h.
 */

#include <kern/sched_prism/waitq.h>

#include <kern/sched_prism/sched.h>

#include <stdbool.h>

void waitq_init(waitq_t *queue)
{
	if (queue == 0) return;

	queue->head = 0;
	queue->tail = 0;
}

void waitq_enqueue(waitq_t *queue, thread_t thread, bool interruptible)
{
	if (queue == 0 || thread == 0) return;

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

thread_t waitq_dequeue(waitq_t *queue)
{
	if (queue == 0) return 0;

	thread_t thread = queue->head;

	if (thread == 0) return 0;

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

	return thread;
}

bool waitq_remove(thread_t thread)
{
	if (thread == 0 || thread->wait_queue == 0) return false;

	waitq_t *queue = thread->wait_queue;
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

	return true;
}

void waitq_wake_one(waitq_t *queue)
{
	thread_t thread = waitq_dequeue(queue);

	if (thread != 0) (void)sched_thread_wakeup(thread);
}

void waitq_wake_all(waitq_t *queue)
{
	thread_t thread;

	while ((thread = waitq_dequeue(queue)) != 0) {
		(void)sched_thread_wakeup(thread);
	}
}

bool waitq_interrupt(thread_t thread)
{
	if (thread == 0 || thread->wait_queue == 0 || !thread->wait_interruptible) return false;

	(void)waitq_remove(thread);
	thread->wait_interrupted = true;

	return sched_thread_wakeup(thread);
}

bool waitq_block(waitq_t *queue, bool interruptible)
{
	thread_t thread = current_thread();

	if (queue == 0 || thread == 0) return false;

	thread->wait_interrupted = false;
	waitq_enqueue(queue, thread, interruptible);

	if (!sched_block(false)) {
		(void)waitq_remove(thread);
		return false;
	}

	/* Woken by whoever made the condition true; make sure we are off the
	 * queue if the scheduler resumed us for any other reason. */
	(void)waitq_remove(thread);

	return !thread->wait_interrupted;
}
