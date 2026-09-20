/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/sched_prism/waitq.h
 *
 * Wait queues: the one way a thread sleeps until something happens.
 *
 * A thread that must wait for a condition (a child to exit, a message to
 * arrive) checks the condition, and if it does not hold, joins a queue and
 * blocks. Whoever makes the condition true wakes the queue. The waiter then
 * checks again -- a wakeup means "look again", never "it is true".
 *
 *     for (;;) {
 *         if (condition()) break;
 *         if (!waitq_block(&queue, true)) return interrupted;
 *     }
 *
 * Two things make this safer than each subsystem keeping its own list:
 *
 *   - A queued thread that is terminated (killed by a signal, its process
 *     exiting) is unlinked first (sched_thread_terminate calls
 *     waitq_remove), so no queue is ever left pointing at a dead thread.
 *
 *   - A wait may be marked interruptible. A signal sent to the process then
 *     unlinks and wakes the thread (waitq_interrupt), and waitq_block reports
 *     it, so the system call can return "interrupted" and let the signal's
 *     handler run.
 *
 * The kernel is not preemptible and wakeups come only from thread context,
 * so checking the condition and joining the queue need no lock on the single
 * CPU. That is an assumption SMP will have to replace with a real lock here.
 */

#ifndef NXU_KERN_SCHED_PRISM_WAITQ_H
#define NXU_KERN_SCHED_PRISM_WAITQ_H

#include <kern/process/thread.h>

#include <stdbool.h>

typedef struct waitq {
	thread_t head;
	thread_t tail;
} waitq_t;

/* An all-zero waitq is valid and empty; this is for readability. */
void waitq_init(waitq_t *queue);

/*
 * waitq_block
 *
 * Put the calling thread on queue and sleep until it is woken. Returns true
 * for an ordinary wakeup, false if a signal interrupted an interruptible
 * wait (the caller returns "interrupted"). Either way the thread is off the
 * queue on return.
 */
bool waitq_block(waitq_t *queue, bool interruptible);

/* Wake the longest-waiting thread / every thread on queue. */
void waitq_wake_one(waitq_t *queue);
void waitq_wake_all(waitq_t *queue);

/*
 * waitq_enqueue / waitq_dequeue
 *
 * The raw halves of waitq_block, for callers (sockets) that must drop their
 * own lock between joining the queue and sleeping. waitq_dequeue removes and
 * returns the oldest waiter without waking it.
 */
void waitq_enqueue(waitq_t *queue, thread_t thread, bool interruptible);
thread_t waitq_dequeue(waitq_t *queue);

/*
 * waitq_remove
 *
 * Unlink thread from whatever queue it is on. Returns false if it was not on
 * one. Called when a thread is terminated.
 */
bool waitq_remove(thread_t thread);

/*
 * waitq_interrupt
 *
 * If thread is in an interruptible wait, take it off its queue, mark the
 * wait interrupted and make it runnable. Returns true if it did.
 */
bool waitq_interrupt(thread_t thread);

#endif
