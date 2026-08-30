#include <kern/sched_prism/run_queue.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * run_queue_priority_valid
 */
static bool run_queue_priority_valid(uint16_t priority)
{
	return priority < RUN_QUEUE_COUNT;
}

/*
 * run_queue_priority_mask
 */
static uint64_t run_queue_priority_mask(uint16_t priority)
{
	return 1ULL << priority;
}

/*
 * run_queue_find_highq
 *
 * Recompute the highest non-empty priority from the bitmap. RUN_QUEUE_COUNT
 * is 64, so the bitmap is one machine word on AArch64.
 */
static uint16_t run_queue_find_highq(uint64_t bitmap)
{
	if (bitmap == 0ULL) return RUN_QUEUE_NONE;

	return (uint16_t)(63U - (uint32_t)__builtin_clzll(bitmap));
}

/*
 * run_queue_init
 */
void run_queue_init(run_queue_t *runq)
{
	if (runq == 0) return;

	memset(runq, 0, sizeof(*runq));
	runq->highq = RUN_QUEUE_NONE;
}

/*
 * run_queue_enqueue
 */
bool run_queue_enqueue(
	run_queue_t *runq,
	thread_t thread,
	run_queue_placement_t placement
)
{
	if (
		runq == 0 ||
		thread == 0 ||
		!thread_is_runnable(thread) ||
		thread_is_idle(thread) ||
		thread->runq != 0 ||
		!run_queue_priority_valid(thread->sched_pri) ||
		(placement != RUN_QUEUE_HEAD && placement != RUN_QUEUE_TAIL)
	) {
		return false;
	}

	uint16_t priority = thread->sched_pri;
	run_queue_bucket_t *bucket = &runq->queues[priority];

	thread->sched_links.runq.prev = 0;
	thread->sched_links.runq.next = 0;

	if (bucket->head == 0) {
		bucket->head = thread;
		bucket->tail = thread;
	} else if (placement == RUN_QUEUE_HEAD) {
		thread->sched_links.runq.next = bucket->head;
		bucket->head->sched_links.runq.prev = thread;
		bucket->head = thread;
	} else {
		thread->sched_links.runq.prev = bucket->tail;
		bucket->tail->sched_links.runq.next = thread;
		bucket->tail = thread;
	}

	thread->runq = runq;
	runq->bitmap |= run_queue_priority_mask(priority);
	runq->count++;

	if (runq->highq == RUN_QUEUE_NONE || runq->highq < priority) runq->highq = priority;

	return true;
}

/*
 * run_queue_remove
 */
bool run_queue_remove(run_queue_t *runq, thread_t thread)
{
	if (
		runq == 0 ||
		thread == 0 ||
		thread->runq != runq ||
		!run_queue_priority_valid(thread->sched_pri)
	) {
		return false;
	}

	uint16_t priority = thread->sched_pri;
	run_queue_bucket_t *bucket = &runq->queues[priority];
	thread_t previous = thread->sched_links.runq.prev;
	thread_t next = thread->sched_links.runq.next;

	if (previous != 0) {
		previous->sched_links.runq.next = next;
	} else if (bucket->head == thread) {
		bucket->head = next;
	} else {
		return false;
	}

	if (next != 0) {
		next->sched_links.runq.prev = previous;
	} else if (bucket->tail == thread) {
		bucket->tail = previous;
	} else {
		return false;
	}

	thread->sched_links.runq.prev = 0;
	thread->sched_links.runq.next = 0;
	thread->runq = 0;

	if (runq->count == 0U) return false;
	runq->count--;

	if (bucket->head == 0) {
		bucket->tail = 0;
		runq->bitmap &= ~run_queue_priority_mask(priority);

		if (runq->highq == priority) {
			runq->highq = run_queue_find_highq(runq->bitmap);
		}
	}

	return true;
}

/*
 * run_queue_dequeue
 */
thread_t run_queue_dequeue(run_queue_t *runq)
{
	if (
		runq == 0 ||
		runq->count == 0U ||
		runq->highq == RUN_QUEUE_NONE ||
		runq->highq >= RUN_QUEUE_COUNT
	) {
		return 0;
	}

	thread_t thread = runq->queues[runq->highq].head;

	if (thread == 0 || !run_queue_remove(runq, thread)) return 0;
	return thread;
}

/*
 * run_queue_peek
 */
thread_t run_queue_peek(const run_queue_t *runq)
{
	if (
		runq == 0 ||
		runq->count == 0U ||
		runq->highq == RUN_QUEUE_NONE ||
		runq->highq >= RUN_QUEUE_COUNT
	) {
		return 0;
	}

	return runq->queues[runq->highq].head;
}

/*
 * run_queue_validate
 */
bool run_queue_validate(const run_queue_t *runq)
{
	if (runq == 0) return false;

	uint64_t bitmap = 0ULL;
	uint32_t count = 0U;

	for (uint16_t priority = 0U; priority < RUN_QUEUE_COUNT; priority++) {
		const run_queue_bucket_t *bucket = &runq->queues[priority];

		if ((bucket->head == 0) != (bucket->tail == 0)) return false;

		thread_t previous = 0;
		uint32_t bucket_count = 0U;

		for (
			thread_t thread = bucket->head;
			thread != 0;
			thread = thread->sched_links.runq.next
		) {
			if (
				thread->runq != runq ||
				thread->sched_pri != priority ||
				thread->sched_links.runq.prev != previous ||
				!thread_is_runnable(thread) ||
				thread_is_idle(thread)
			) {
				return false;
			}

			previous = thread;
			bucket_count++;
			count++;

			if (bucket_count > THREAD_MAX || count > THREAD_MAX) return false;
		}

		if (previous != bucket->tail) return false;


		if (bucket_count != 0U) bitmap |= run_queue_priority_mask(priority);
	}

	if (count != runq->count || bitmap != runq->bitmap) return false;
	return runq->highq == run_queue_find_highq(bitmap);
}
