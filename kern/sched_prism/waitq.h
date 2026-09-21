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
 * SMP. Each queue has its own lock, taken with interrupts masked, and it
 * covers the queue links and thread->wait_queue. waitq_block joins the queue
 * and marks the thread waiting under that lock, so a waker on another CPU
 * (which must take it to dequeue) can only find the thread once it is fully
 * "waiting", and its wakeup is then never lost or applied to a thread that has
 * not gone to sleep yet.
 *
 * That closes the window inside waitq_block, but not the one in the loop
 * above: `condition()` is checked before the lock is taken. A subsystem whose
 * waker can run on another CPU than the sleeper (which today means none:
 * threads of the existing subsystems are all pinned to the boot CPU, see
 * processor_default_affinity) must check the condition under a lock its waker
 * also takes, or use waitq_block_unlock() (kern/sched_prism/waitq.h).
 */

#ifndef NXU_KERN_SCHED_PRISM_WAITQ_H
#define NXU_KERN_SCHED_PRISM_WAITQ_H

#include <kern/lock.h>
#include <kern/process/thread.h>

#include <stdbool.h>

typedef struct waitq {
	thread_t head;
	thread_t tail;
	nxu_spinlock_t lock;
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

/*
 * waitq_block_unlock
 *
 * waitq_block for a caller that checked its condition under `guard`, a lock its
 * waker also takes: joins the queue and marks the thread waiting *before*
 * releasing `guard`, so the waker, who needs `guard` to make the condition true,
 * cannot wake a thread that is not yet asleep. `guard` is released on return
 * either way, and interrupts are restored to what they were before the caller
 * took it (pass that state in `irq_state`).
 */
bool waitq_block_unlock(waitq_t *queue, bool interruptible, nxu_spinlock_t *guard, uint64_t irq_state);

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
