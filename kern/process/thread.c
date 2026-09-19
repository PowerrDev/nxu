#include <kern/console/console.h>
#include <kern/process/thread.h>
#include <kern/process/task.h>

#include <mach/machine/cpu.h>

#include <platform/uart.h>
#include <vm/vm_kern.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Thread objects are allocated from a fixed boot-time pool. The pool keeps
 * thread lifetime independent of the general heap and gives the scheduler a
 * bounded object set during early kernel development.
 */
typedef struct {
	volatile uint32_t value;
} thread_lock_t;

static thread_lock_t g_thread_lock;

static struct thread g_thread_slots[THREAD_MAX];
static bool g_thread_slot_used[THREAD_MAX];

static thread_t g_threads_head;
static thread_t g_threads_tail;
static thread_t g_current_thread;

static thread_id_t g_next_thread_id = 1ULL;
static uint32_t g_thread_count;
static bool g_thread_initialized;

/*
 * thread_lock
 *
 * Serialize thread-object, task-membership and global-list state.
 */
static void thread_lock(thread_lock_t *lock)
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
 * thread_unlock
 */
static void thread_unlock(thread_lock_t *lock)
{
	__atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

/*
 * thread_reset_locked
 *
 * Initialize one thread slot to the detached inactive state.
 */
static void thread_reset_locked(thread_t thread, uint32_t slot)
{
	*thread = (struct thread) {
		.threads_prev = 0,
		.threads_next = 0,
		.task_prev = 0,
		.task_next = 0,
		.sched_links = {
			.runq = {
				.prev = 0,
				.next = 0
			}
		},
		.runq = 0,
		.task = 0,
		.thread_id = THREAD_ID_INVALID,
		.ref_count = 0U,
		.state = 0U,
		.flags = TH_FLAG_NONE,
		.active = false,
		.started = false,
		.sched_pri = THREAD_PRIORITY_DEFAULT,
		.base_pri = THREAD_PRIORITY_DEFAULT,
		.max_priority = THREAD_PRIORITY_MAX,
		.mlfq_level = 0U,
		.suspend_count = 0U,
		.quantum_remaining = 0U,
		.continuation = 0,
		.parameter = 0,
		.kernel_stack = 0,
		.kernel_stack_size = 0ULL,
		.machine = { 0 },
		.slot = slot
	};
}

/*
 * thread_allocate_slot_locked
 *
 * Reserve one object from the static thread pool.
 */
static thread_t thread_allocate_slot_locked(void)
{
	for (uint32_t index = 0U; index < THREAD_MAX; index++) {
		if (g_thread_slot_used[index]) continue;

		g_thread_slot_used[index] = true;
		thread_reset_locked(&g_thread_slots[index], index);
		return &g_thread_slots[index];
	}

	return 0;
}

/*
 * thread_release_slot_locked
 *
 * Return a fully-detached thread object to the allocator.
 */
static void thread_release_slot_locked(thread_t thread)
{
	if (thread == 0 || thread->slot >= THREAD_MAX) return;

	uint32_t slot = thread->slot;

	if (&g_thread_slots[slot] != thread) return;

	g_thread_slot_used[slot] = false;
	thread_reset_locked(&g_thread_slots[slot], slot);
}

/*
 * thread_allocate_id_locked
 *
 * Allocate a monotonically increasing thread identifier. Zero and
 * THREAD_ID_INVALID are never returned.
 */
static thread_id_t thread_allocate_id_locked(void)
{
	for (;;) {
		thread_id_t thread_id = g_next_thread_id++;

		if (
			thread_id != 0ULL &&
			thread_id != THREAD_ID_INVALID
		) {
			return thread_id;
		}
	}
}

/*
 * thread_global_insert_locked
 */
static void thread_global_insert_locked(thread_t thread)
{
	thread->threads_prev = g_threads_tail;
	thread->threads_next = 0;

	if (g_threads_tail != 0) {
		g_threads_tail->threads_next = thread;
	} else {
		g_threads_head = thread;
	}

	g_threads_tail = thread;
	g_thread_count++;
}

/*
 * thread_global_remove_locked
 */
static void thread_global_remove_locked(thread_t thread)
{
	if (thread->threads_prev != 0) {
		thread->threads_prev->threads_next = thread->threads_next;
	} else {
		g_threads_head = thread->threads_next;
	}

	if (thread->threads_next != 0) {
		thread->threads_next->threads_prev = thread->threads_prev;
	} else {
		g_threads_tail = thread->threads_prev;
	}

	thread->threads_prev = 0;
	thread->threads_next = 0;

	if (g_thread_count != 0U) g_thread_count--;
}

/*
 * thread_task_insert_locked
 *
 * Attach thread to the owning task. The task list owns one residency
 * reference until thread termination removes the object.
 */
static void thread_task_insert_locked(task_t task, thread_t thread)
{
	thread->task = task;
	thread->task_prev = 0;
	thread->task_next = task->threads;

	if (task->threads != 0) task->threads->task_prev = thread;

	task->threads = thread;
	task->thread_count++;
	task->active_thread_count++;
}

/*
 * thread_task_remove_locked
 */
static void thread_task_remove_locked(thread_t thread)
{
	task_t task = thread->task;

	if (task == 0) return;


	if (thread->task_prev != 0) {
		thread->task_prev->task_next = thread->task_next;
	} else {
		task->threads = thread->task_next;
	}

	if (thread->task_next != 0) {
		thread->task_next->task_prev = thread->task_prev;
	}

	if (task->thread_count != 0U) task->thread_count--;
	if (thread->active && task->active_thread_count != 0U) task->active_thread_count--;

	thread->task_prev = 0;
	thread->task_next = 0;
	thread->task = 0;
}

/*
 * thread_reference_locked
 */
static bool thread_reference_locked(thread_t thread)
{
	if (
		thread == 0 ||
		thread->ref_count == 0U ||
		thread->ref_count == UINT32_MAX
	) {
		return false;
	}

	thread->ref_count++;
	return true;
}

/*
 * thread_create_common
 *
 * Allocate and attach one thread object. The task membership owns the first
 * reference and the caller receives a second reference through result.
 */
static bool thread_create_common(
	task_t task,
	bool kernel_thread,
	uint64_t entry,
	uint64_t stack,
	uint64_t user_arg,
	thread_continue_t continuation,
	void *parameter,
	thread_t *result
)
{
	if (task == 0 || result == 0 || !task_is_active(task)) return false;

	if (!kernel_thread && (entry == 0ULL || stack == 0ULL)) return false;
	if (kernel_thread && continuation == 0) return false;

	*result = 0;

	if (!g_thread_initialized && !thread_bootstrap()) return false;

	thread_lock(&g_thread_lock);

	thread_t thread = thread_allocate_slot_locked();

	if (thread == 0) {
		thread_unlock(&g_thread_lock);
		return false;
	}

	thread->thread_id = thread_allocate_id_locked();
	thread->ref_count = 1U;
	thread->state = TH_SUSP;
	thread->flags = kernel_thread ? TH_FLAG_KERNEL : TH_FLAG_NONE;
	thread->active = true;
	thread->started = false;
	thread->suspend_count = 1U;
	thread->continuation = continuation;
	thread->parameter = parameter;

	bool machine_initialized;

	if (kernel_thread) {
		machine_thread_init_kernel(&thread->machine);
		machine_initialized = true;
	} else {
		machine_initialized = machine_thread_init_user(
			&thread->machine,
			entry,
			stack,
			user_arg
		);
	}

	if (!machine_initialized) {
		thread_release_slot_locked(thread);
		thread_unlock(&g_thread_lock);
		return false;
	}

	thread_task_insert_locked(task, thread);
	thread_global_insert_locked(thread);

	if (!thread_reference_locked(thread)) {
		thread_task_remove_locked(thread);
		thread_global_remove_locked(thread);
		thread_release_slot_locked(thread);
		thread_unlock(&g_thread_lock);
		return false;
	}

	*result = thread;

	thread_unlock(&g_thread_lock);
	return true;
}

/*
 * thread_bootstrap
 *
 * Initialize the thread allocator and global lists.
 */
bool thread_bootstrap(void)
{
	thread_lock(&g_thread_lock);

	if (g_thread_initialized) {
		thread_unlock(&g_thread_lock);
		return true;
	}

	for (uint32_t index = 0U; index < THREAD_MAX; index++) {
		g_thread_slot_used[index] = false;
		thread_reset_locked(&g_thread_slots[index], index);
	}

	g_threads_head = 0;
	g_threads_tail = 0;
	g_current_thread = 0;
	g_next_thread_id = 1ULL;
	g_thread_count = 0U;
	g_thread_initialized = true;

	thread_unlock(&g_thread_lock);
	return true;
}

/*
 * thread_create
 */
bool thread_create(
	task_t task,
	uint64_t entry,
	uint64_t stack,
	uint64_t arg,
	thread_t *result
)
{
	if (task_is_kernel(task)) return false;

	return thread_create_common(
		task,
		false,
		entry,
		stack,
		arg,
		0,
		0,
		result
	);
}

/*
 * kernel_thread_create
 */
bool kernel_thread_create(
	task_t task,
	thread_continue_t continuation,
	void *parameter,
	thread_t *result
)
{
	if (!task_is_kernel(task)) return false;

	return thread_create_common(
		task,
		true,
		0ULL,
		0ULL,
		0ULL,
		continuation,
		parameter,
		result
	);
}

/*
 * thread_create_bootstrap
 *
 * Create the thread object representing the already-running kernel boot
 * context. The first real context switch will save its current SP and
 * callee-saved registers into machine.context.
 */
bool thread_create_bootstrap(task_t task, thread_t *result)
{
	if (
		task == 0 ||
		result == 0 ||
		!task_is_active(task) ||
		!task_is_kernel(task)
	) {
		return false;
	}

	*result = 0;

	if (!g_thread_initialized && !thread_bootstrap()) return false;

	thread_lock(&g_thread_lock);

	thread_t thread = thread_allocate_slot_locked();

	if (thread == 0) {
		thread_unlock(&g_thread_lock);
		return false;
	}

	thread->thread_id = thread_allocate_id_locked();
	thread->ref_count = 1U;
	thread->state = TH_RUN;
	thread->flags = TH_FLAG_KERNEL | TH_FLAG_BOOTSTRAP;
	thread->active = true;
	thread->started = true;
	thread->suspend_count = 0U;

	machine_thread_init_kernel(&thread->machine);

	thread_task_insert_locked(task, thread);
	thread_global_insert_locked(thread);

	if (!thread_reference_locked(thread)) {
		thread_task_remove_locked(thread);
		thread_global_remove_locked(thread);
		thread_release_slot_locked(thread);
		thread_unlock(&g_thread_lock);
		return false;
	}

	*result = thread;

	thread_unlock(&g_thread_lock);
	return true;
}

/*
 * thread_reference
 */
bool thread_reference(thread_t thread)
{
	if (thread == 0) return false;

	thread_lock(&g_thread_lock);

	bool result = thread_reference_locked(thread);

	thread_unlock(&g_thread_lock);
	return result;
}

/*
 * thread_deallocate
 *
 * Release one caller reference. A live thread retains the independent
 * residency reference owned by its task membership. A terminated thread
 * retains a termination residency reference until thread_reap().
 */
void thread_deallocate(thread_t thread)
{
	if (thread == 0) return;

	thread_lock(&g_thread_lock);

	uint32_t residency =
		(thread->task != 0 ||
		((thread->state & TH_TERMINATE) != 0U &&
		(thread->flags & TH_FLAG_REAPED) == 0U))
			? 1U
			: 0U;

	if (thread->ref_count > residency) thread->ref_count--;

	if (
		thread->ref_count == 0U &&
		!thread->active &&
		thread->task == 0
	) {
		thread_release_slot_locked(thread);
	}

	thread_unlock(&g_thread_lock);
}

/*
 * thread_start
 *
 * Release the creation hold and make a new thread runnable.
 */
bool thread_start(thread_t thread)
{
	if (thread == 0) return false;

	thread_lock(&g_thread_lock);

	bool valid =
		thread->active &&
		!thread->started &&
		(thread->state & TH_TERMINATE) == 0U &&
		thread->suspend_count != 0U;

	if (valid) {
		thread->started = true;
		thread->suspend_count--;

		if (thread->suspend_count == 0U) {
			thread->state &= ~TH_SUSP;
			thread->state |= TH_RUN;
		}
	}

	thread_unlock(&g_thread_lock);
	return valid;
}

/*
 * thread_wait
 *
 * Move a runnable thread into wait state. The scheduler will later remove
 * runq ownership before calling this transition.
 */
bool thread_wait(thread_t thread, bool uninterruptible)
{
	if (thread == 0) return false;

	thread_lock(&g_thread_lock);

	bool valid =
		thread->active &&
		thread->started &&
		thread->runq == 0 &&
		(thread->state & TH_TERMINATE) == 0U &&
		(thread->state & TH_WAIT) == 0U;

	if (valid) {
		thread->state &= ~TH_RUN;
		thread->state |= TH_WAIT;

		if (uninterruptible) {
			thread->state |= TH_UNINT;
		} else {
			thread->state &= ~TH_UNINT;
		}
	}

	thread_unlock(&g_thread_lock);
	return valid;
}

/*
 * thread_go
 *
 * Wake a waiting thread. A suspended thread is made non-waiting but is not
 * runnable until its final suspension hold is released.
 */
bool thread_go(thread_t thread)
{
	if (thread == 0) return false;

	thread_lock(&g_thread_lock);

	bool valid =
		thread->active &&
		(thread->state & TH_WAIT) != 0U &&
		(thread->state & TH_TERMINATE) == 0U;

	if (valid) {
		thread->state &= ~(TH_WAIT | TH_UNINT | TH_WAKING);

		if (thread->suspend_count == 0U) thread->state |= TH_RUN;
	}

	thread_unlock(&g_thread_lock);
	return valid;
}

/*
 * thread_hold
 *
 * Add a kernel suspension hold. A held thread is not runnable.
 */
bool thread_hold(thread_t thread)
{
	if (thread == 0) return false;

	thread_lock(&g_thread_lock);

	bool valid =
		thread->active &&
		thread->runq == 0 &&
		(thread->state & TH_TERMINATE) == 0U &&
		thread->suspend_count != UINT32_MAX;

	if (valid) {
		thread->suspend_count++;
		thread->state |= TH_SUSP;
		thread->state &= ~TH_RUN;
	}

	thread_unlock(&g_thread_lock);
	return valid;
}

/*
 * thread_release
 *
 * Drop one kernel suspension hold.
 */
bool thread_release(thread_t thread)
{
	if (thread == 0) return false;

	thread_lock(&g_thread_lock);

	bool valid =
		thread->active &&
		thread->suspend_count != 0U &&
		(thread->state & TH_TERMINATE) == 0U;

	if (valid) {
		thread->suspend_count--;

		if (thread->suspend_count == 0U) {
			thread->state &= ~TH_SUSP;

			if (
				thread->started &&
				(thread->state & TH_WAIT) == 0U
			) {
				thread->state |= TH_RUN;
			}
		}
	}

	thread_unlock(&g_thread_lock);
	return valid;
}

/*
 * thread_terminate
 *
 * Remove a thread from scheduling eligibility and detach it from the task.
 * Kernel-stack reclamation is deliberately deferred to thread_reap() so a
 * thread can terminate while it is still executing on its own EL1 stack.
 */
bool thread_terminate(thread_t thread)
{
	if (thread == 0) return false;

	thread_lock(&g_thread_lock);

	if (
		!thread->active ||
		thread->runq != 0 ||
		(thread->state & TH_TERMINATE) != 0U
	) {
		thread_unlock(&g_thread_lock);
		return false;
	}

	thread->state = TH_TERMINATE;
	thread->started = false;
	thread->sched_links.runq.prev = 0;
	thread->sched_links.runq.next = 0;

	thread_task_remove_locked(thread);
	thread_global_remove_locked(thread);

	thread->active = false;

	/*
	 * The old task-membership reference becomes a termination residency
	 * reference. thread_reap() drops it after the thread is no longer
	 * current on a processor.
	 */

	thread_unlock(&g_thread_lock);
	return true;
}

/*
 * thread_reap
 *
 * Release the termination residency reference and reclaim a dead thread's
 * kernel stack after it is no longer executing.
 */
bool thread_reap(thread_t thread)
{
	if (thread == 0) return false;

	thread_lock(&g_thread_lock);

	if (
		thread->active ||
		thread->task != 0 ||
		(thread->state & TH_TERMINATE) == 0U ||
		g_current_thread == thread ||
		thread->ref_count == 0U
	) {
		thread_unlock(&g_thread_lock);
		return false;
	}

	void *stack = thread->kernel_stack;
	uint64_t stack_size = thread->kernel_stack_size;

	thread->kernel_stack = 0;
	thread->kernel_stack_size = 0ULL;
	thread->machine.context.sp = 0ULL;

	/*
	 * Drop the termination residency reference.
	 */
	thread->ref_count--;
	thread->flags |= TH_FLAG_REAPED;

	bool release_slot = thread->ref_count == 0U;

	thread_unlock(&g_thread_lock);

	if (stack != 0 && stack_size != 0ULL) {
		if (!vm_kern_free(stack, (size_t)stack_size)) return false;
	}

	if (release_slot) {
		thread_lock(&g_thread_lock);

		if (
			thread->ref_count == 0U &&
			!thread->active &&
			thread->task == 0 &&
			thread->kernel_stack == 0
		) {
			thread_release_slot_locked(thread);
		}

		thread_unlock(&g_thread_lock);
	}

	return true;
}

/*
 * thread_stack_alloc
 *
 * Allocate the EL1 stack used while this thread executes kernel code.
 */
bool thread_stack_alloc(thread_t thread)
{
	if (thread == 0) return false;

	thread_lock(&g_thread_lock);

	bool valid =
		thread->active &&
		thread->kernel_stack == 0 &&
		(thread->state & TH_TERMINATE) == 0U;

	thread_unlock(&g_thread_lock);

	if (!valid) return false;

	void *stack = 0;

	if (!vm_kern_allocate(
		(size_t)THREAD_KERNEL_STACK_SIZE,
		VMM_PROTECTION_READ_WRITE,
		&stack
	)) {
		return false;
	}

	memset(stack, 0, (size_t)THREAD_KERNEL_STACK_SIZE);

	thread_lock(&g_thread_lock);

	if (
		!thread->active ||
		thread->kernel_stack != 0 ||
		(thread->state & TH_TERMINATE) != 0U
	) {
		thread_unlock(&g_thread_lock);
		return vm_kern_free(
			stack,
			(size_t)THREAD_KERNEL_STACK_SIZE
		);
	}

	thread->kernel_stack = stack;
	thread->kernel_stack_size = THREAD_KERNEL_STACK_SIZE;
	thread->machine.context.sp = (uint64_t)stack + THREAD_KERNEL_STACK_SIZE;

	thread_unlock(&g_thread_lock);
	return true;
}

/*
 * thread_stack_free
 */
bool thread_stack_free(thread_t thread)
{
	if (thread == 0) return false;

	thread_lock(&g_thread_lock);

	if (
		thread->kernel_stack == 0 ||
		g_current_thread == thread
	) {
		thread_unlock(&g_thread_lock);
		return false;
	}

	void *stack = thread->kernel_stack;
	uint64_t size = thread->kernel_stack_size;

	thread->kernel_stack = 0;
	thread->kernel_stack_size = 0ULL;
	thread->machine.context.sp = 0ULL;

	thread_unlock(&g_thread_lock);
	return vm_kern_free(stack, (size_t)size);
}

/*
 * thread_set_current
 *
 * Establish the single-CPU current thread. The scheduler replaces this
 * global pointer with processor-local scheduler state.
 */
bool thread_set_current(thread_t thread)
{
	if (thread == 0) return false;

	thread_lock(&g_thread_lock);

	bool valid =
		thread->active &&
		thread->started &&
		(thread->state & TH_RUN) != 0U &&
		(thread->state &
			(TH_WAIT | TH_SUSP | TH_TERMINATE)) == 0U;

	if (valid) {
		__atomic_store_n(
			&g_current_thread,
			thread,
			__ATOMIC_RELEASE
		);
	}

	thread_unlock(&g_thread_lock);
	return valid;
}

/*
 * current_thread
 */
thread_t current_thread(void)
{
	return __atomic_load_n(
		&g_current_thread,
		__ATOMIC_ACQUIRE
	);
}

/*
 * task_first_thread_ref
 */
thread_t task_first_thread_ref(task_t task)
{
	if (task == 0) return 0;

	thread_lock(&g_thread_lock);

	thread_t thread = task->threads;

	if (
		thread != 0 &&
		!thread_reference_locked(thread)
	) {
		thread = 0;
	}

	thread_unlock(&g_thread_lock);
	return thread;
}

/*
 * thread_next_task_thread_ref
 */
thread_t thread_next_task_thread_ref(thread_t thread)
{
	if (thread == 0) return 0;

	thread_lock(&g_thread_lock);

	thread_t next = thread->task_next;

	if (
		next != 0 &&
		!thread_reference_locked(next)
	) {
		next = 0;
	}

	thread_unlock(&g_thread_lock);
	return next;
}

/*
 * thread_tid
 */
thread_id_t thread_tid(thread_t thread)
{
	if (thread == 0) return THREAD_ID_INVALID;
	return thread->thread_id;
}

/*
 * thread_task
 */
task_t thread_task(thread_t thread)
{
	if (thread == 0) return 0;
	return thread->task;
}

/*
 * task_active_thread_count
 *
 * Unlike task->active_thread_count itself, this is safe to call once a task
 * can have more than one thread: reads are serialized against the same
 * g_thread_lock thread_task_insert_locked/thread_task_remove_locked take to
 * mutate the field.
 */
uint32_t task_active_thread_count(task_t task)
{
	if (task == 0) return 0U;

	thread_lock(&g_thread_lock);
	uint32_t count = task->active_thread_count;
	thread_unlock(&g_thread_lock);

	return count;
}

/*
 * thread_is_active
 */
bool thread_is_active(thread_t thread)
{
	return thread != 0 && thread->active;
}

/*
 * thread_is_started
 */
bool thread_is_started(thread_t thread)
{
	return thread != 0 && thread->started;
}

/*
 * thread_is_runnable
 */
bool thread_is_runnable(thread_t thread)
{
	return (
		thread != 0 &&
		thread->active &&
		(thread->state & TH_RUN) != 0U &&
		(thread->state &
			(TH_WAIT | TH_SUSP | TH_TERMINATE)) == 0U
	);
}

/*
 * thread_is_waiting
 */
bool thread_is_waiting(thread_t thread)
{
	return (
		thread != 0 &&
		(thread->state & TH_WAIT) != 0U
	);
}

/*
 * thread_is_suspended
 */
bool thread_is_suspended(thread_t thread)
{
	return (
		thread != 0 &&
		(thread->state & TH_SUSP) != 0U
	);
}

/*
 * thread_is_terminated
 */
bool thread_is_terminated(thread_t thread)
{
	return (
		thread != 0 &&
		(thread->state & TH_TERMINATE) != 0U
	);
}

/*
 * thread_is_idle
 */
bool thread_is_idle(thread_t thread)
{
	return thread != 0 && (thread->state & TH_IDLE) != 0U;
}

/*
 * thread_is_bootstrap
 */
bool thread_is_bootstrap(thread_t thread)
{
	return thread != 0 && (thread->flags & TH_FLAG_BOOTSTRAP) != 0U;
}

/*
 * thread_set_idle
 *
 * Mark an unstarted kernel thread as the processor idle thread.
 */
bool thread_set_idle(thread_t thread)
{
	if (thread == 0) return false;

	thread_lock(&g_thread_lock);

	bool valid =
		thread->active &&
		!thread->started &&
		thread->runq == 0 &&
		(thread->flags & TH_FLAG_KERNEL) != 0U &&
		(thread->flags & TH_FLAG_BOOTSTRAP) == 0U &&
		(thread->state & TH_TERMINATE) == 0U;

	if (valid) thread->state |= TH_IDLE;

	thread_unlock(&g_thread_lock);
	return valid;
}

/*
 * thread_set_priority
 *
 * Change a detached scheduling priority. Queued threads are repositioned by
 * sched_thread_set_priority() so run-queue bucket invariants stay intact.
 */
bool thread_set_priority(thread_t thread, uint16_t priority)
{
	if (
		thread == 0 ||
		priority < THREAD_PRIORITY_MIN ||
		priority > THREAD_PRIORITY_MAX
	) {
		return false;
	}

	thread_lock(&g_thread_lock);

	bool valid =
		thread->active &&
		thread->runq == 0 &&
		(thread->state & TH_TERMINATE) == 0U &&
		priority <= thread->max_priority;

	if (valid) {
		thread->base_pri = priority;
		thread->sched_pri = priority;
	}

	thread_unlock(&g_thread_lock);
	return valid;
}

/*
 * thread_priority
 */
uint16_t thread_priority(thread_t thread)
{
	return thread == 0 ? 0U : thread->sched_pri;
}

/*
 * thread_base_priority
 */
uint16_t thread_base_priority(thread_t thread)
{
	return thread == 0 ? 0U : thread->base_pri;
}

/*
 * thread_user_entry
 */
uint64_t thread_user_entry(thread_t thread)
{
	if (
		thread == 0 ||
		!machine_thread_has_user_state(&thread->machine)
	) {
		return 0ULL;
	}

	return machine_thread_user_pc(&thread->machine);
}

/*
 * thread_user_stack
 */
uint64_t thread_user_stack(thread_t thread)
{
	if (
		thread == 0 ||
		!machine_thread_has_user_state(&thread->machine)
	) {
		return 0ULL;
	}

	return machine_thread_user_sp(&thread->machine);
}

/*
 * thread_set_user_state
 */
bool thread_set_user_state(
	thread_t thread,
	uint64_t entry,
	uint64_t stack,
	uint64_t arg
)
{
	if (
		thread == 0 ||
		(thread->flags & TH_FLAG_KERNEL) != 0U
	) {
		return false;
	}

	thread_lock(&g_thread_lock);

	bool valid =
		thread->active &&
		(thread->state & TH_TERMINATE) == 0U;

	if (valid) {
		valid = machine_thread_set_user_state(
			&thread->machine,
			entry,
			stack,
			arg
		);
	}

	thread_unlock(&g_thread_lock);
	return valid;
}

/*
 * thread_kernel_stack_base
 */
uint64_t thread_kernel_stack_base(thread_t thread)
{
	if (thread == 0) return 0ULL;
	return (uint64_t)thread->kernel_stack;
}

/*
 * thread_kernel_stack_top
 */
uint64_t thread_kernel_stack_top(thread_t thread)
{
	if (
		thread == 0 ||
		thread->kernel_stack == 0
	) {
		return 0ULL;
	}

	return
		(uint64_t)thread->kernel_stack +
		thread->kernel_stack_size;
}

/*
 * thread_count
 */
uint32_t thread_count(void)
{
	thread_lock(&g_thread_lock);

	uint32_t count = g_thread_count;

	thread_unlock(&g_thread_lock);
	return count;
}

/*
 * thread_validate
 *
 * Validate global linkage, task back-pointers and task thread counters.
 */
bool thread_validate(void)
{
	thread_lock(&g_thread_lock);

	uint32_t global_count = 0U;
	thread_t previous = 0;

	for (
		thread_t thread = g_threads_head;
		thread != 0;
		thread = thread->threads_next
	) {
		if (
			thread->threads_prev != previous ||
			!thread->active ||
			thread->task == 0 ||
			thread->ref_count == 0U ||
			thread->slot >= THREAD_MAX ||
			!g_thread_slot_used[thread->slot]
		) {
			thread_unlock(&g_thread_lock);
			return false;
		}

		bool found_in_task = false;
		uint32_t task_count = 0U;
		uint32_t active_count = 0U;

		for (
			thread_t member = thread->task->threads;
			member != 0;
			member = member->task_next
		) {
			task_count++;
			if (member->active) active_count++;

			if (member == thread) found_in_task = true;

			if (task_count > THREAD_MAX) {
				thread_unlock(&g_thread_lock);
				return false;
			}
		}

		if (
			!found_in_task ||
			task_count != thread->task->thread_count ||
			active_count != thread->task->active_thread_count
		) {
			thread_unlock(&g_thread_lock);
			return false;
		}

		previous = thread;
		global_count++;

		if (global_count > THREAD_MAX) {
			thread_unlock(&g_thread_lock);
			return false;
		}
	}

	bool valid =
		previous == g_threads_tail &&
		global_count == g_thread_count;

	thread_unlock(&g_thread_lock);
	return valid;
}

/*
 * thread_print_state
 *
 * Print thread state without requiring a returned static string pointer.
 */
static void thread_print_state(uint32_t state)
{
	bool printed = false;

	if ((state & TH_RUN) != 0U) {
		kputs("run");
		printed = true;
	}

	if ((state & TH_WAIT) != 0U) {
		if (printed) kputc('|');

		kputs("wait");
		printed = true;
	}

	if ((state & TH_SUSP) != 0U) {
		if (printed) kputc('|');

		kputs("susp");
		printed = true;
	}

	if ((state & TH_UNINT) != 0U) {
		if (printed) kputc('|');

		kputs("unint");
		printed = true;
	}

	if ((state & TH_TERMINATE) != 0U) {
		if (printed) kputc('|');

		kputs("terminate");
		printed = true;
	}

	if ((state & TH_IDLE) != 0U) {
		if (printed) kputc('|');

		kputs("idle");
		printed = true;
	}

	if (!printed) kputs("new");
}

/*
 * thread_dump
 */
void thread_dump(void)
{
	thread_lock(&g_thread_lock);

	kputln("thread: global thread list");

	for (
		thread_t thread = g_threads_head;
		thread != 0;
		thread = thread->threads_next
	) {
		kputs("thread: tid ");
		kputu64(thread->thread_id);

		kputs(", task ");
		kputu64(
			thread->task != 0
				? thread->task->task_uniqueid
				: 0ULL
		);

		kputs(", state ");
		thread_print_state(thread->state);

		kputs(", refs ");
		kputu64(thread->ref_count);

		kputs(", priority ");
		kputu64(thread->sched_pri);

		if (!thread_is_idle(thread)) {
			kputs(", mlfq level ");
			kputu64(thread->mlfq_level);
		}

		kputc('\n');
	}

	thread_unlock(&g_thread_lock);
}
