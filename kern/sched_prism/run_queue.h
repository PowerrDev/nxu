#ifndef NXU_KERN_RUN_QUEUE_H
#define NXU_KERN_RUN_QUEUE_H

#include <kern/process/thread.h>

#include <stdbool.h>
#include <stdint.h>

#define RUN_QUEUE_COUNT (THREAD_PRIORITY_MAX + 1U)
#define RUN_QUEUE_NONE UINT16_MAX

typedef struct {
	thread_t head;
	thread_t tail;
} run_queue_bucket_t;

typedef struct run_queue {
	run_queue_bucket_t queues[RUN_QUEUE_COUNT];
	uint64_t bitmap;
	uint32_t count;
	uint16_t highq;
} run_queue_t;

typedef enum {
	RUN_QUEUE_HEAD,
	RUN_QUEUE_TAIL
} run_queue_placement_t;

/*
 * run_queue_init
 *
 * Initialize an empty fixed-priority run queue.
 *
 * Run-queue operations do not provide their own lock. The scheduler owns
 * serialization so one scheduler lock can protect both processor state and
 * queue membership without nested queue locks.
 */
void run_queue_init(run_queue_t *runq);

/*
 * run_queue_enqueue
 *
 * Insert a runnable thread at the head or tail of its scheduling-priority
 * bucket. Higher numeric priorities are selected before lower priorities.
 */
bool run_queue_enqueue(
	run_queue_t *runq,
	thread_t thread,
	run_queue_placement_t placement
);

/*
 * run_queue_dequeue
 *
 * Remove and return the highest-priority runnable thread. Threads at equal
 * priority are selected FIFO when callers enqueue at RUN_QUEUE_TAIL.
 */
thread_t run_queue_dequeue(run_queue_t *runq);

/*
 * run_queue_remove
 *
 * Remove one specific thread from its current run queue.
 */
bool run_queue_remove(run_queue_t *runq, thread_t thread);

/*
 * run_queue_peek
 *
 * Inspect the next thread without removing it.
 */
thread_t run_queue_peek(const run_queue_t *runq);

/*
 * run_queue_dequeue_priority
 *
 * Remove and return the head thread queued at exactly one priority bucket,
 * independent of runq->highq. Feedback policies use this to drain one
 * specific level (for example, everything below the top MLFQ queue during a
 * priority boost) without disturbing threads at other priorities.
 */
thread_t run_queue_dequeue_priority(run_queue_t *runq, uint16_t priority);

/*
 * run_queue_validate
 *
 * Validate queue linkage, bitmap state, priority placement and accounting.
 */
bool run_queue_validate(const run_queue_t *runq);

#endif
