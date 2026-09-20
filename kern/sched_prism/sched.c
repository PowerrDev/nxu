#include <kern/console/console.h>
#include <kern/sched_prism/sched.h>
#include <kern/sched_prism/waitq.h>
#include <kern/machine/cpu.h>
#include <kern/machine/machine_routines.h>
#include <kern/process/proc.h>
#include <platform/uart.h>
#include <vm/address_space.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	volatile uint32_t value;
} sched_lock_t;

typedef struct {
	sched_lock_t lock;
	thread_t bootstrap_thread;
	thread_t idle_thread;
	bool initialized;
	uint32_t mlfq_boost_ticks;
} sched_state_t;

typedef enum {
	SCHED_SWITCH_YIELD,
	SCHED_SWITCH_BLOCK,
	SCHED_SWITCH_EXIT,
	SCHED_SWITCH_PREEMPT
} sched_switch_reason_t;

static sched_state_t g_sched;

/*
 * sched_lock
 */
static void sched_lock(sched_lock_t *lock)
{
	while (
		__atomic_exchange_n(
			&lock->value,
			1U,
			__ATOMIC_ACQUIRE
		) != 0U
	) {
		cpu_relax();
	}
}

/*
 * sched_unlock
 */
static void sched_unlock(sched_lock_t *lock)
{
	__atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

/*
 * sched_fatal
 */
static __attribute__((noreturn))
void sched_fatal(const char *message)
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
static const uint16_t sched_mlfq_priority[SCHED_MLFQ_LEVELS] = {
	63U, 47U, 31U, 15U
};

static const uint32_t sched_mlfq_quantum[SCHED_MLFQ_LEVELS] = {
	4U, 8U, 16U, 32U
};

_Static_assert(
	sizeof(sched_mlfq_priority) / sizeof(sched_mlfq_priority[0]) == SCHED_MLFQ_LEVELS,
	"sched_mlfq_priority must have exactly SCHED_MLFQ_LEVELS entries"
);

_Static_assert(
	sizeof(sched_mlfq_quantum) / sizeof(sched_mlfq_quantum[0]) == SCHED_MLFQ_LEVELS,
	"sched_mlfq_quantum must have exactly SCHED_MLFQ_LEVELS entries"
);

uint16_t sched_mlfq_level_priority(uint8_t level)
{
	if (level > SCHED_MLFQ_BOTTOM_LEVEL) level = SCHED_MLFQ_BOTTOM_LEVEL;
	return sched_mlfq_priority[level];
}

uint32_t sched_mlfq_level_quantum(uint8_t level)
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
static void sched_mlfq_assign(thread_t thread, uint8_t level)
{
	if (thread == 0 || thread_is_idle(thread)) return;

	if (level > SCHED_MLFQ_BOTTOM_LEVEL) level = SCHED_MLFQ_BOTTOM_LEVEL;

	thread->mlfq_level = level;
	thread->sched_pri = sched_mlfq_priority[level];
	thread->base_pri = sched_mlfq_priority[level];
	thread->quantum_remaining = sched_mlfq_quantum[level];
}

/*
 * sched_mlfq_boost_locked
 *
 * The "equalizer" rule: drain every level below the top queue and put its
 * threads back at level 0 with a fresh top-level quantum, then do the same
 * for the currently running thread. Called with g_sched.lock held.
 */
static void sched_mlfq_boost_locked(processor_t processor)
{
	if (processor == 0) return;

	for (
		uint8_t level = SCHED_MLFQ_TOP_LEVEL + 1U;
		level < SCHED_MLFQ_LEVELS;
		level++
	) {
		uint16_t priority = sched_mlfq_priority[level];
		thread_t thread;

		while (
			(thread = run_queue_dequeue_priority(&processor->runq, priority)) != 0
		) {
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
 * sched_quantum_reset
 *
 * The idle thread keeps a fixed placeholder quantum; it is never charged by
 * sched_tick() and never appears in the run queue. Every other thread's
 * quantum comes from its current MLFQ level.
 */
static void sched_quantum_reset(thread_t thread)
{
	if (thread == 0) return;

	thread->quantum_remaining = thread_is_idle(thread)
		? SCHED_DEFAULT_QUANTUM_TICKS
		: sched_mlfq_level_quantum(thread->mlfq_level);
}

/*
 * sched_queue_placement
 */
static run_queue_placement_t sched_queue_placement(
	sched_queue_placement_t placement
)
{
	return placement == SCHED_HEADQ ? RUN_QUEUE_HEAD : RUN_QUEUE_TAIL;
}

/*
 * sched_activate_thread
 *
 * Install the address-space and BSD process identity associated with the
 * incoming thread before its kernel context becomes current.
 */
static bool sched_activate_thread(thread_t thread)
{
	if (thread == 0 || thread->task == 0) return false;

	task_t task = thread->task;
	proc_t proc = task_get_proc(task);

	if (proc == 0) return false;

	bool address_space_ready = task_is_kernel(task)
		? vm_address_space_deactivate()
		: task_activate_address_space(task);

	if (!address_space_ready) return false;


	if (current_proc() != proc && !proc_set_current(proc)) return false;
	return true;
}

/*
 * sched_finish_switch
 *
 * Complete the handoff after the incoming stack is live. A terminating
 * outgoing thread can only be reaped at this point because its old EL1 stack
 * is no longer executing.
 */
static void sched_finish_switch(void)
{
	uint64_t irq_state = ml_irq_save();
	processor_t processor = current_processor();

	sched_lock(&g_sched.lock);

	thread_t previous = processor->previous_thread;
	processor->previous_thread = 0;

	sched_unlock(&g_sched.lock);

	if (
		previous != 0 &&
		previous != current_thread() &&
		thread_is_terminated(previous)
	) {
		(void)thread_reap(previous);
	}

	ml_irq_restore(irq_state);
}

/*
 * sched_thread_continue
 *
 * First C instruction executed by a never-run thread after its synthetic
 * machine context is restored.
 */
static __attribute__((noreturn))
void sched_thread_continue(void)
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
	 * The current SYS_exit path terminates the task before redirecting ERET
	 * through arm64_return_from_el0. Any active thread reaching this point is
	 * therefore an invalid scheduler transition.
	 */
	if (thread_is_active(thread)) {
		sched_fatal("thread_is_active: active user thread returned to first-run trampoline");
	}

	sched_exit_current();
}

/*
 * sched_prepare_new_thread
 */
static bool sched_prepare_new_thread(thread_t thread)
{
	if (thread == 0 || thread_is_bootstrap(thread)) return thread != 0;

	if (thread->kernel_stack == 0 && !thread_stack_alloc(thread)) return false;

	return machine_thread_prepare_context(
		&thread->machine,
		thread_kernel_stack_top(thread),
		(uint64_t)sched_thread_continue
	);
}

/*
 * sched_idle_continue
 */
static __attribute__((noreturn))
void sched_idle_continue(void *parameter)
{
	(void)parameter;

	for (;;) {
		cpu_wait_for_interrupt();
	}
}

/*
 * sched_switch
 *
 * Select and activate an incoming thread, then switch SP_EL1 and the AArch64
 * callee-saved context. IRQs remain masked across the scheduler handoff so
 * processor->active_thread can never disagree with the stack actually in use.
 */
static bool sched_switch(sched_switch_reason_t reason)
{
	if (!g_sched.initialized) return false;

	uint64_t irq_state = ml_irq_save();
	processor_t processor = current_processor();

	if (processor == 0) {
		ml_irq_restore(irq_state);
		return false;
	}

	sched_lock(&g_sched.lock);

	thread_t current = processor->active_thread;

	if (current == 0) {
		sched_unlock(&g_sched.lock);
		ml_irq_restore(irq_state);
		return false;
	}

	if (reason == SCHED_SWITCH_PREEMPT) {
		thread_t candidate = run_queue_peek(&processor->runq);

		if (
			candidate == 0 ||
			(!thread_is_idle(current) &&
			candidate->sched_pri < current->sched_pri)
		) {
			processor->preemption_pending = false;
			sched_quantum_reset(current);
			sched_unlock(&g_sched.lock);
			ml_irq_restore(irq_state);
			return true;
		}
	}

	bool requeue_current =
		(reason == SCHED_SWITCH_YIELD || reason == SCHED_SWITCH_PREEMPT) &&
		thread_is_active(current) &&
		thread_is_runnable(current) &&
		!thread_is_idle(current);

	if (
		requeue_current &&
		!run_queue_enqueue(&processor->runq, current, RUN_QUEUE_TAIL)
	) {
		sched_unlock(&g_sched.lock);
		ml_irq_restore(irq_state);
		return false;
	}

	thread_t next = run_queue_dequeue(&processor->runq);

	if (next == 0) next = processor->idle_thread;

	if (next == 0) {
		sched_unlock(&g_sched.lock);
		ml_irq_restore(irq_state);
		return false;
	}

	if (next == current) {
		processor->next_thread = 0;
		processor->preemption_pending = false;
		sched_quantum_reset(current);
		sched_unlock(&g_sched.lock);
		ml_irq_restore(irq_state);
		return true;
	}

	processor->next_thread = next;
	processor->dispatch_count++;

	sched_unlock(&g_sched.lock);

	if (!sched_activate_thread(next)) {
		sched_fatal("sched_activate_thread: incoming thread activation failed");
	}

	sched_lock(&g_sched.lock);

	if (!thread_set_current(next)) {
		sched_unlock(&g_sched.lock);
		sched_fatal("thread_set_current: current-thread handoff failed");
	}

	processor->previous_thread = current;
	processor->active_thread = next;
	processor->next_thread = 0;
	processor->state = thread_is_idle(next)
		? PROCESSOR_IDLE
		: PROCESSOR_RUNNING;
	processor->preemption_pending = false;
	processor->context_switch_count++;

	if (reason == SCHED_SWITCH_PREEMPT) {
		processor->preemption_count++;
	}

	sched_quantum_reset(next);

	sched_unlock(&g_sched.lock);

	machine_thread_switch_context(
		&current->machine,
		&next->machine
	);

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
bool sched_bootstrap(task_t kernel_task)
{
	if (g_sched.initialized) return true;

	if (kernel_task == 0 || !task_is_kernel(kernel_task)) return false;
	if (!processor_bootstrap()) return false;

	memset(&g_sched, 0, sizeof(g_sched));

	thread_t bootstrap_thread;

	if (!thread_create_bootstrap(kernel_task, &bootstrap_thread)) return false;

	if (!thread_set_current(bootstrap_thread)) {
		thread_deallocate(bootstrap_thread);
		return false;
	}

	thread_t idle_thread;

	if (!kernel_thread_create(
		kernel_task,
		sched_idle_continue,
		0,
		&idle_thread
	)) {
		thread_deallocate(bootstrap_thread);
		return false;
	}

	if (
		!thread_set_idle(idle_thread) ||
		!thread_stack_alloc(idle_thread) ||
		!machine_thread_prepare_context(
			&idle_thread->machine,
			thread_kernel_stack_top(idle_thread),
			(uint64_t)sched_thread_continue
		) ||
		!thread_start(idle_thread)
	) {
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
	processor->state = PROCESSOR_RUNNING;

	g_sched.bootstrap_thread = bootstrap_thread;
	g_sched.idle_thread = idle_thread;
	g_sched.initialized = true;

	return true;
}

bool sched_is_initialized(void)
{
	return g_sched.initialized;
}

thread_t sched_bootstrap_thread(void)
{
	return g_sched.initialized ? g_sched.bootstrap_thread : 0;
}

thread_t sched_idle_thread(void)
{
	return g_sched.initialized ? g_sched.idle_thread : 0;
}

bool thread_setrun(thread_t thread, sched_queue_placement_t placement)
{
	if (
		!g_sched.initialized ||
		thread == 0 ||
		thread_is_idle(thread) ||
		!thread_is_runnable(thread) ||
		(placement != SCHED_HEADQ && placement != SCHED_TAILQ)
	) {
		return false;
	}

	uint64_t irq_state = ml_irq_save();
	processor_t processor = current_processor();

	sched_lock(&g_sched.lock);

	bool valid =
		thread != processor->active_thread &&
		thread->runq == 0 &&
		run_queue_enqueue(
			&processor->runq,
			thread,
			sched_queue_placement(placement)
		);

	if (valid && processor->active_thread != 0) {
		if (
			thread_is_idle(processor->active_thread) ||
			thread->sched_pri > processor->active_thread->sched_pri
		) {
			processor->preemption_pending = true;
		}
	}

	sched_unlock(&g_sched.lock);
	ml_irq_restore(irq_state);
	return valid;
}

bool thread_run_queue_remove(thread_t thread)
{
	if (!g_sched.initialized || thread == 0) return false;

	uint64_t irq_state = ml_irq_save();
	sched_lock(&g_sched.lock);

	bool result =
		thread->runq != 0 &&
		run_queue_remove(thread->runq, thread);

	sched_unlock(&g_sched.lock);
	ml_irq_restore(irq_state);
	return result;
}

thread_t thread_select(processor_t processor)
{
	if (!g_sched.initialized || processor == 0) return 0;

	uint64_t irq_state = ml_irq_save();
	sched_lock(&g_sched.lock);

	thread_t thread = run_queue_dequeue(&processor->runq);

	if (thread == 0) thread = processor->idle_thread;

	if (thread != 0) {
		processor->next_thread = thread;
		processor->dispatch_count++;
		sched_quantum_reset(thread);
	}

	sched_unlock(&g_sched.lock);
	ml_irq_restore(irq_state);
	return thread;
}

bool sched_thread_start(thread_t thread)
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

bool sched_thread_wait(thread_t thread, bool uninterruptible)
{
	if (thread == 0) return false;

	if (thread->runq != 0 && !thread_run_queue_remove(thread)) return false;
	return thread_wait(thread, uninterruptible);
}

bool sched_thread_wakeup(thread_t thread)
{
	if (thread == 0 || !thread_go(thread)) return false;

	if (thread_is_runnable(thread) && !thread_is_idle(thread)) {
		return thread_setrun(thread, SCHED_TAILQ);
	}

	return true;
}

bool sched_thread_hold(thread_t thread)
{
	if (thread == 0) return false;

	if (thread->runq != 0 && !thread_run_queue_remove(thread)) return false;
	return thread_hold(thread);
}

bool sched_thread_release(thread_t thread)
{
	if (thread == 0 || !thread_release(thread)) return false;

	if (thread_is_runnable(thread) && !thread_is_idle(thread)) {
		return thread_setrun(thread, SCHED_TAILQ);
	}

	return true;
}

bool sched_thread_terminate(thread_t thread)
{
	if (thread == 0) return false;

	/* Before thread_terminate clobbers the links they share with the run
	 * queue: a sleeping thread must leave its wait queue first. */
	(void)waitq_remove(thread);

	if (thread->runq != 0 && !thread_run_queue_remove(thread)) return false;
	return thread_terminate(thread);
}

bool sched_thread_set_priority(thread_t thread, uint16_t priority)
{
	if (
		thread == 0 ||
		priority < THREAD_PRIORITY_MIN ||
		priority > THREAD_PRIORITY_MAX ||
		thread_is_idle(thread)
	) {
		return false;
	}

	bool queued = thread->runq != 0;

	if (queued && !thread_run_queue_remove(thread)) return false;


	if (!thread_set_priority(thread, priority)) {
		if (queued) (void)thread_setrun(thread, SCHED_TAILQ);
		return false;
	}

	if (queued) return thread_setrun(thread, SCHED_TAILQ);

	uint64_t irq_state = ml_irq_save();
	processor_t processor = current_processor();

	sched_lock(&g_sched.lock);

	thread_t candidate = run_queue_peek(&processor->runq);

	if (
		thread == processor->active_thread &&
		candidate != 0 &&
		candidate->sched_pri > thread->sched_pri
	) {
		processor->preemption_pending = true;
	}

	sched_unlock(&g_sched.lock);
	ml_irq_restore(irq_state);
	return true;
}

bool sched_yield(void)
{
	return sched_switch(SCHED_SWITCH_YIELD);
}

bool sched_block(bool uninterruptible)
{
	thread_t thread = current_thread();

	if (
		thread == 0 ||
		thread_is_idle(thread) ||
		!sched_thread_wait(thread, uninterruptible)
	) {
		return false;
	}

	return sched_switch(SCHED_SWITCH_BLOCK);
}

__attribute__((noreturn))
void sched_exit_current(void)
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

void sched_tick(void)
{
	if (!g_sched.initialized) return;

	uint64_t irq_state = ml_irq_save();
	processor_t processor = current_processor();

	sched_lock(&g_sched.lock);

	thread_t thread = processor->active_thread;

	if (
		thread != 0 &&
		thread_is_active(thread) &&
		!thread_is_idle(thread)
	) {
		if (thread->quantum_remaining != 0U) thread->quantum_remaining--;

		if (thread->quantum_remaining == 0U) {
			processor->quantum_expiration_count++;
			processor->preemption_pending = true;

			/*
			 * Rule 4, the demotion: the thread ran through its entire
			 * quantum without voluntarily giving up the CPU (blocking or
			 * yielding resets the quantum without ever reaching zero
			 * here), so treat it as CPU-bound and drop it one MLFQ level.
			 * sched_mlfq_assign() also grants the new level's quantum, so
			 * there is no separate sched_quantum_reset() call here.
			 */
			uint8_t level = thread->mlfq_level;

			if (level < SCHED_MLFQ_BOTTOM_LEVEL) level++;
			sched_mlfq_assign(thread, level);
		}
	}

	/*
	 * Rule 5, the equalizer boost. This runs on wall-clock ticks regardless
	 * of which thread is active (including while the CPU is idle) so
	 * background work queued during a quiet period is not left waiting at
	 * the bottom the moment something else shows up.
	 */
	g_sched.mlfq_boost_ticks++;

	if (g_sched.mlfq_boost_ticks >= SCHED_MLFQ_BOOST_INTERVAL_TICKS) {
		g_sched.mlfq_boost_ticks = 0U;
		sched_mlfq_boost_locked(processor);
	}

	sched_unlock(&g_sched.lock);
	ml_irq_restore(irq_state);
}

bool sched_preempt(void)
{
	if (!sched_preemption_pending()) return true;
	return sched_switch(SCHED_SWITCH_PREEMPT);
}

bool sched_preemption_pending(void)
{
	if (!g_sched.initialized) return false;

	processor_t processor = current_processor();
	return __atomic_load_n(&processor->preemption_pending, __ATOMIC_ACQUIRE);
}

void sched_clear_preemption(void)
{
	if (!g_sched.initialized) return;

	processor_t processor = current_processor();
	__atomic_store_n(&processor->preemption_pending, false, __ATOMIC_RELEASE);
}

static void sched_test_thread(struct thread *thread, uint16_t priority)
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

bool sched_run_queue_self_test(void)
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

/*
 * sched_mlfq_test_thread
 */
static void sched_mlfq_test_thread(struct thread *thread, uint8_t level)
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

bool sched_mlfq_self_test(void)
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
	if (hog.mlfq_level != SCHED_MLFQ_TOP_LEVEL) return false;
	if (hog.sched_pri != sched_mlfq_level_priority(SCHED_MLFQ_TOP_LEVEL)) return false;

	/*
	 * Rule 4: a CPU-bound thread that keeps exhausting its quantum drops
	 * one level per exhaustion until it reaches the bottom queue, and
	 * never falls off the bottom.
	 */
	for (uint8_t level = SCHED_MLFQ_TOP_LEVEL; level < SCHED_MLFQ_BOTTOM_LEVEL; level++) {
		if (hog.mlfq_level != level) return false;
		if (hog.quantum_remaining != sched_mlfq_level_quantum(level)) return false;

		sched_mlfq_assign(&hog, (uint8_t)(hog.mlfq_level + 1U));
	}

	if (hog.mlfq_level != SCHED_MLFQ_BOTTOM_LEVEL) return false;

	sched_mlfq_assign(&hog, (uint8_t)(hog.mlfq_level + 1U));
	if (hog.mlfq_level != SCHED_MLFQ_BOTTOM_LEVEL) return false;

	/*
	 * Rule 5: threads parked below the top queue, and the currently
	 * running thread, all return to level 0 on a boost.
	 */
	sched_mlfq_test_thread(&queued_mid, SCHED_MLFQ_BOTTOM_LEVEL - 1U);
	sched_mlfq_test_thread(&queued_bottom, SCHED_MLFQ_BOTTOM_LEVEL);
	sched_mlfq_test_thread(&running, SCHED_MLFQ_BOTTOM_LEVEL);

	if (!run_queue_enqueue(&test_processor.runq, &queued_mid, RUN_QUEUE_TAIL)) return false;
	if (!run_queue_enqueue(&test_processor.runq, &queued_bottom, RUN_QUEUE_TAIL)) return false;

	test_processor.active_thread = &running;

	sched_mlfq_boost_locked(&test_processor);

	if (queued_mid.mlfq_level != SCHED_MLFQ_TOP_LEVEL) return false;
	if (queued_bottom.mlfq_level != SCHED_MLFQ_TOP_LEVEL) return false;
	if (running.mlfq_level != SCHED_MLFQ_TOP_LEVEL) return false;

	if (queued_mid.sched_pri != sched_mlfq_level_priority(SCHED_MLFQ_TOP_LEVEL)) return false;
	if (queued_mid.runq != &test_processor.runq) return false;
	if (queued_bottom.runq != &test_processor.runq) return false;

	return run_queue_validate(&test_processor.runq);
}

bool sched_validate(void)
{
	if (!g_sched.initialized) return false;

	processor_t processor = current_processor();
	thread_t current = current_thread();

	if (
		processor == 0 ||
		current == 0 ||
		processor->active_thread != current ||
		processor->idle_thread != g_sched.idle_thread ||
		!thread_is_bootstrap(g_sched.bootstrap_thread) ||
		!thread_is_idle(g_sched.idle_thread) ||
		g_sched.idle_thread->runq != 0 ||
		processor->previous_thread != 0 ||
		!processor_validate(processor)
	) {
		return false;
	}

	return true;
}

void sched_dump(void)
{
	if (!g_sched.initialized) {
		kputln("sched_dump: not initialized");
		return;
	}

	processor_t processor = current_processor();

	kprintf("sched_dump: cpu: %llu\n", (unsigned long long)processor->cpu_id);
	kprintf("sched_dump: current tid: %llu\n", (unsigned long long)thread_tid(processor->active_thread));
	kprintf("sched_dump: idle tid: %llu\n", (unsigned long long)thread_tid(processor->idle_thread));
	kprintf("sched_dump: run queue count: %llu\n", (unsigned long long)processor->runq.count);
	kprintf("sched_dump: context switches: %llu\n", (unsigned long long)processor->context_switch_count);

	if (!kconsole_verbose()) return;

	if (processor->runq.highq == RUN_QUEUE_NONE) {
		kverbosef("sched_dump: run queue highq: none\n");
	} else {
		kverbosef("sched_dump: run queue highq: %llu\n", (unsigned long long)processor->runq.highq);
	}

	kverbosef("sched_dump: dispatches: %llu\n", (unsigned long long)processor->dispatch_count);
	kverbosef("sched_dump: preemptions: %llu\n", (unsigned long long)processor->preemption_count);
	kverbosef("sched_dump: quantum expirations: %llu\n", (unsigned long long)processor->quantum_expiration_count);
	kverbosef("sched_dump: default quantum: %llu tick(s)\n", (unsigned long long)SCHED_DEFAULT_QUANTUM_TICKS);
	kverbosef("sched_dump: MLFQ levels: %llu\n", (unsigned long long)SCHED_MLFQ_LEVELS);
	kverbosef("sched_dump: MLFQ boost interval: %llu tick(s)\n", (unsigned long long)SCHED_MLFQ_BOOST_INTERVAL_TICKS);
	kverbosef("sched_dump: MLFQ next boost in: %llu tick(s)\n", (unsigned long long)(SCHED_MLFQ_BOOST_INTERVAL_TICKS - g_sched.mlfq_boost_ticks));

	if (
		processor->active_thread != 0 &&
		!thread_is_idle(processor->active_thread)
	) {
		kverbosef("sched_dump: active thread MLFQ level: %llu\n", (unsigned long long)processor->active_thread->mlfq_level);
		kverbosef("sched_dump: active thread quantum remaining: %llu\n", (unsigned long long)processor->active_thread->quantum_remaining);
	}
}
