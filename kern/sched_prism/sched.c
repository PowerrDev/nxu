/*
 * File:        kern/sched_prism/sched.c
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
 * g_thread_lock. sched_lock is a leaf that is never held together with a
 * runq_lock. Code that needs two run queues (migration, balancing) takes them
 * in ascending logical CPU id, and no path holds a runq_lock while it waits
 * for anything else. A CPU takes only its own runq_lock in the hot path
 * (sched_switch, sched_tick); another CPU's is taken only to place or migrate
 * a thread onto it.
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

#include <kern/console/console.h>
#include <kern/sched_prism/sched.h>
#include <kern/sched_prism/waitq.h>
#include <kern/ipi.h>
#include <kern/machine/cpu.h>
#include <kern/machine/machine_routines.h>
#include <kern/machine/smp.h>
#include <kern/process/proc.h>
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

typedef enum {
	SCHED_SWITCH_YIELD,
	SCHED_SWITCH_BLOCK,
	SCHED_SWITCH_EXIT,
	SCHED_SWITCH_PREEMPT
} sched_switch_reason_t;

static sched_state_t g_sched;

/* Every this many ticks an idle or underloaded CPU looks for work to pull from a busier one. */
#define SCHED_BALANCE_INTERVAL_TICKS 4U

static bool sched_switch(sched_switch_reason_t reason);
static processor_t sched_select_cpu(thread_t thread);
static bool sched_enqueue_on(processor_t target, thread_t thread, run_queue_placement_t placement);

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
static bool sched_mlfq_charge_tick(thread_t thread, bool *quantum_expired)
{
	if (thread->quantum_remaining != 0U) thread->quantum_remaining--;
	if (thread->mlfq_ticks != UINT32_MAX) thread->mlfq_ticks++;

	bool expired = thread->quantum_remaining == 0U;
	bool spent = thread->mlfq_level < SCHED_MLFQ_BOTTOM_LEVEL && thread->mlfq_ticks >= sched_mlfq_quantum[thread->mlfq_level];

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
static void sched_mlfq_blocked(thread_t thread)
{
	thread->mlfq_ticks = 0U;
}

/*
 * sched_mlfq_boost_locked
 *
 * The "equalizer" rule: drain every level below the top queue and put its
 * threads back at level 0 with a fresh top-level quantum, then do the same
 * for the currently running thread. Called with the CPU's runq_lock held.
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

	/*
	 * Address-space and process identity are boot-CPU state so far (a single
	 * TTBR0 bookkeeping and one "current process"): secondary CPUs run kernel
	 * threads only, with TTBR0 disabled from the moment they came up, and have
	 * nothing to switch. sched_cpu_may_run() keeps user threads off them.
	 */
	if (current_processor()->cpu_id != 0U) return task_is_kernel(task);
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

	/* previous_thread and previous_requeue are this CPU's own: written before the switch, read here. */
	thread_t previous = processor->previous_thread;
	bool requeue = processor->previous_requeue;

	processor->previous_thread = 0;
	processor->previous_requeue = false;

	if (previous == 0 || previous == current_thread()) {
		ml_irq_restore(irq_state);
		return;
	}

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
	} else if (
		(requeue || woken) &&
		!thread_is_idle(previous) &&
		thread_is_runnable(previous) &&
		previous->runq == 0
	) {
		processor_t target = sched_select_cpu(previous);

		if (!sched_enqueue_on(target, previous, RUN_QUEUE_TAIL)) {
			sched_fatal("sched_finish_switch: could not requeue the outgoing thread");
		}
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
 * sched_cpu_may_run
 *
 * Whether `thread` may run on `cpu`: its affinity allows it, and the CPU can
 * run this kind of thread. Threads of user tasks stay on the boot CPU: the
 * address-space bookkeeping (which space is live in TTBR0, the current process)
 * is single-CPU state, so a secondary CPU never runs user code yet.
 */
static bool sched_cpu_may_run(thread_t thread, uint32_t cpu)
{
	if (!cpuset_contains(&thread->affinity, cpu)) return false;
	if (cpu != 0U && (thread->flags & TH_FLAG_KERNEL) == 0U) return false;
	return true;
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
static processor_t sched_select_cpu(thread_t thread)
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
static bool sched_enqueue_on(processor_t target, thread_t thread, run_queue_placement_t placement)
{
	processor_t self = current_processor();
	uint64_t irq_state = ml_irq_save();
	bool kick = false;

	nxu_spin_lock(&target->runq_lock);

	bool queued = thread->runq == 0 && run_queue_enqueue(&target->runq, thread, placement);

	if (queued) {
		thread_t active = target->active_thread;

		if (thread->last_cpu != target->cpu_id) target->migrate_in_count++;

		if (
			active == 0 ||
			thread_is_idle(active) ||
			thread->sched_pri > active->sched_pri
		) {
			kick = !__atomic_exchange_n(&target->preemption_pending, true, __ATOMIC_ACQ_REL);
		}
	}

	nxu_spin_unlock(&target->runq_lock);

	if (kick && target != self) ipi_send(target->cpu_id, IPI_RESCHEDULE);

	ml_irq_restore(irq_state);
	return queued;
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
static bool sched_switch(sched_switch_reason_t reason)
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
		yielding &&
		thread_is_active(current) &&
		thread_is_runnable(current) &&
		!thread_is_idle(current);

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
			stay =
				sched_cpu_may_run(current, processor->cpu_id) &&
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

	thread_t next = run_queue_dequeue(&processor->runq);

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

	if (!sched_activate_thread(next)) {
		sched_fatal("sched_activate_thread: incoming thread activation failed");
	}

	nxu_spin_lock(&processor->runq_lock);

	if (!thread_set_current(next)) {
		nxu_spin_unlock(&processor->runq_lock);
		sched_fatal("thread_set_current: current-thread handoff failed");
	}

	processor->previous_thread = current;
	processor->previous_requeue = requeue_current;
	processor->next_thread = 0;

	__atomic_store_n(
		&processor->state,
		thread_is_idle(next) ? PROCESSOR_IDLE : PROCESSOR_RUNNING,
		__ATOMIC_RELEASE
	);

	__atomic_store_n(&processor->preemption_pending, false, __ATOMIC_RELEASE);
	processor->context_switch_count++;

	if (reason == SCHED_SWITCH_PREEMPT) {
		processor->preemption_count++;
	}

	sched_quantum_reset(next);

	nxu_spin_unlock(&processor->runq_lock);

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
	processor->previous_requeue = false;
	processor->state = PROCESSOR_RUNNING;

	bootstrap_thread->on_cpu = processor;
	bootstrap_thread->last_cpu = processor->cpu_id;

	g_sched.bootstrap_thread = bootstrap_thread;
	g_sched.kernel_task = kernel_task;
	g_sched.initialized = true;

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
bool sched_cpu_prepare(uint32_t cpu)
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
__attribute__((noreturn))
void sched_cpu_idle(void)
{
	processor_t processor = current_processor();

	__atomic_store_n(&processor->state, PROCESSOR_IDLE, __ATOMIC_RELEASE);

	for (;;) {
		ml_irq_enable();
		cpu_wait_for_interrupt();
	}
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
	processor_t processor = g_sched.initialized ? current_processor() : 0;

	return processor != 0 ? processor->idle_thread : 0;
}

bool sched_thread_can_run_on(thread_t thread, uint32_t cpu)
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
bool thread_run_queue_remove(thread_t thread)
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

thread_t thread_select(processor_t processor)
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

	/* A thread lowering its own priority may now be beaten by something queued on its CPU. */
	uint64_t irq_state = nxu_spin_lock_irqsave(&current_processor()->runq_lock);
	processor_t processor = current_processor();

	thread_t candidate = run_queue_peek(&processor->runq);

	if (
		thread == processor->active_thread &&
		candidate != 0 &&
		candidate->sched_pri > thread->sched_pri
	) {
		__atomic_store_n(&processor->preemption_pending, true, __ATOMIC_RELEASE);
	}

	nxu_spin_unlock_irqrestore(&processor->runq_lock, irq_state);
	return true;
}

bool sched_yield(void)
{
	return sched_switch(SCHED_SWITCH_YIELD);
}

bool sched_block_commit(void)
{
	return sched_switch(SCHED_SWITCH_BLOCK);
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

	return sched_block_commit();
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
static bool sched_migrate_one(processor_t from, processor_t to)
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
static void sched_balance(processor_t self)
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

void sched_tick(void)
{
	if (!g_sched.initialized) return;

	uint64_t irq_state = ml_irq_save();
	processor_t processor = current_processor();

	nxu_spin_lock(&processor->runq_lock);

	thread_t thread = processor->active_thread;

	if (
		thread != 0 &&
		thread_is_active(thread) &&
		!thread_is_idle(thread)
	) {
		bool quantum_expired;

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

	nxu_spin_unlock(&processor->runq_lock);

	if (processor->ticks % SCHED_BALANCE_INTERVAL_TICKS == 0U) sched_balance(processor);

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
	 * A thread that yields every tick never runs out its quantum (each
	 * dispatch refills it) but still spends its level's allotment: it sinks
	 * one level per quantum's worth of ticks, and takes no earlier than that.
	 */
	sched_mlfq_test_thread(&hog, SCHED_MLFQ_TOP_LEVEL);

	for (uint8_t level = SCHED_MLFQ_TOP_LEVEL; level < SCHED_MLFQ_BOTTOM_LEVEL; level++) {
		uint32_t ticks = sched_mlfq_level_quantum(level);

		for (uint32_t tick = 1U; tick <= ticks; tick++) {
			if (hog.mlfq_level != level) return false;

			bool demoted = sched_mlfq_charge_tick(&hog, 0);

			if (demoted != (tick == ticks)) return false;

			/* The yield: the next dispatch grants a whole quantum again. */
			if (!demoted) hog.quantum_remaining = sched_mlfq_level_quantum(level);
		}

		if (hog.mlfq_level != level + 1U) return false;
		if (hog.mlfq_ticks != 0U) return false;
	}

	/* The bottom level has nowhere lower to go. */
	for (uint32_t tick = 0U; tick < sched_mlfq_level_quantum(SCHED_MLFQ_BOTTOM_LEVEL) * 2U; tick++) {
		(void)sched_mlfq_charge_tick(&hog, 0);
		if (hog.mlfq_level != SCHED_MLFQ_BOTTOM_LEVEL) return false;
	}

	/* Sleeping clears the allotment, so a thread that blocks between short bursts stays where it is. */
	sched_mlfq_test_thread(&hog, SCHED_MLFQ_TOP_LEVEL);

	for (uint32_t round = 0U; round < 8U; round++) {
		for (uint32_t tick = 1U; tick < sched_mlfq_level_quantum(SCHED_MLFQ_TOP_LEVEL); tick++) {
			if (sched_mlfq_charge_tick(&hog, 0)) return false;

			hog.quantum_remaining = sched_mlfq_level_quantum(SCHED_MLFQ_TOP_LEVEL);
		}

		sched_mlfq_blocked(&hog);
	}

	if (hog.mlfq_level != SCHED_MLFQ_TOP_LEVEL) return false;

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
		processor->idle_thread == 0 ||
		!thread_is_bootstrap(g_sched.bootstrap_thread) ||
		!thread_is_idle(processor->idle_thread) ||
		processor->idle_thread->runq != 0 ||
		processor->previous_thread != 0 ||
		!processor_validate(processor)
	) {
		return false;
	}

	return true;
}

/*
 * sched_validate_all
 *
 * The cross-CPU invariants, checked with every CPU's run queue locked in
 * ascending id order: each queued thread is on exactly the queue its runq
 * pointer names, is runnable, is not running anywhere, may run on the CPU that
 * holds it, and appears on no other queue. Costly (it walks every queue), so
 * only tests call it.
 */
bool sched_validate_all(void)
{
	if (!g_sched.initialized) return false;

	uint64_t irq_state = ml_irq_save();
	uint32_t count = processor_count();
	bool ok = true;

	for (uint32_t cpu = 0U; cpu < count; cpu++) nxu_spin_lock(&processor_by_id(cpu)->runq_lock);

	for (uint32_t cpu = 0U; cpu < count && ok; cpu++) {
		processor_t processor = processor_by_id(cpu);

		if (!run_queue_validate(&processor->runq)) ok = false;

		for (uint32_t priority = 0U; priority < RUN_QUEUE_COUNT && ok; priority++) {
			for (thread_t thread = processor->runq.queues[priority].head; thread != 0 && ok; thread = thread->sched_links.runq.next) {
				if (
					thread->runq != &processor->runq ||
					thread_is_idle(thread) ||
					!thread_is_runnable(thread) ||
					thread->on_cpu != 0 ||
					!sched_cpu_may_run(thread, cpu)
				) {
					ok = false;
				}

				/* On no other queue: a thread queued twice would have two runq owners. */
				for (uint32_t other = cpu + 1U; other < count && ok; other++) {
					for (uint32_t level = 0U; level < RUN_QUEUE_COUNT && ok; level++) {
						for (thread_t peer = processor_by_id(other)->runq.queues[level].head; peer != 0; peer = peer->sched_links.runq.next) {
							if (peer == thread) ok = false;
						}
					}
				}
			}
		}
	}

	for (uint32_t cpu = count; cpu > 0U; cpu--) nxu_spin_unlock(&processor_by_id(cpu - 1U)->runq_lock);

	ml_irq_restore(irq_state);
	return ok;
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
	kverbosef("sched_dump: MLFQ next boost in: %llu tick(s)\n", (unsigned long long)(SCHED_MLFQ_BOOST_INTERVAL_TICKS - processor->boost_ticks));

	if (
		processor->active_thread != 0 &&
		!thread_is_idle(processor->active_thread)
	) {
		kverbosef("sched_dump: active thread MLFQ level: %llu\n", (unsigned long long)processor->active_thread->mlfq_level);
		kverbosef("sched_dump: active thread quantum remaining: %llu\n", (unsigned long long)processor->active_thread->quantum_remaining);
	}
}
