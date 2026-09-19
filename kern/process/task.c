#include <kern/process/task.h>
#include <kern/sched_prism/sched.h>
#include <kern/process/thread.h>
#include <vm/vm_map.h>
#include <vm/vm_shm.h>

#include <stdint.h>

#define TASK_PRIORITY_DEFAULT 31U
#define TASK_PRIORITY_MAX 63U

/*
 * task_reset
 *
 * Reset a task object before initialization.
 */
static void task_reset(task_t task)
{
	*task = (struct task) {
		.task_uniqueid = 0ULL,
		.ref_count = 0U,
		.state = TASK_STATE_INACTIVE,
		.flags = TASK_FLAG_NONE,
		.active = false,
		.halting = false,
		.map = {
			.root = 0,
			.root_physical = 0ULL,
			.table_count = 0ULL,
			.active = false
		},
		.shm_next_va = 0ULL,
		.threads = 0,
		.thread_count = 0U,
		.active_thread_count = 0U,
		.suspend_count = 0U,
		.priority = TASK_PRIORITY_DEFAULT,
		.max_priority = TASK_PRIORITY_MAX,
		.bsd_info = 0
	};
}

/*
 * task_init_kernel
 *
 * Initialize the kernel task. Kernel threads may later be attached to this
 * task but it does not own a userspace address space.
 */
bool task_init_kernel(
	task_t task,
	uint64_t uniqueid,
	struct proc *bsd_info
)
{
	if (task == 0 || bsd_info == 0) return false;

	task_reset(task);

	task->task_uniqueid = uniqueid;
	task->ref_count = 1U;
	task->state = TASK_STATE_ACTIVE;
	task->flags = TASK_FLAG_KERNEL;
	task->active = true;
	task->bsd_info = bsd_info;

	return true;
}

/*
 * task_init_user
 *
 * Initialize a userspace task and create the first thread which will execute
 * in its address space.
 */
bool task_init_user(
	task_t task,
	uint64_t uniqueid,
	struct proc *bsd_info,
	uint64_t entry,
	uint64_t stack
)
{
	if (
		task == 0 ||
		bsd_info == 0 ||
		entry == 0ULL ||
		stack == 0ULL
	) {
		return false;
	}

	task_reset(task);

	if (!vm_address_space_create(&task->map)) return false;

	task->shm_next_va = VM_SHM_BASE;
	task->task_uniqueid = uniqueid;
	task->ref_count = 1U;
	task->state = TASK_STATE_ACTIVE;
	task->flags = TASK_FLAG_NONE;
	task->active = true;
	task->bsd_info = bsd_info;

	thread_t initial_thread;

	if (!thread_create(
		task,
		entry,
		stack,
		0ULL,
		&initial_thread
	)) {
		task->active = false;
		task->state = TASK_STATE_TERMINATED;
		return false;
	}

	/*
	 * thread_create() returns an additional caller reference. The task's
	 * thread membership keeps the thread resident after this reference is
	 * dropped.
	 */
	thread_deallocate(initial_thread);

	return true;
}

/*
 * task_reference
 */
bool task_reference(task_t task)
{
	if (
		task == 0 ||
		task->ref_count == UINT32_MAX ||
		task->state == TASK_STATE_TERMINATED
	) {
		return false;
	}

	task->ref_count++;
	return true;
}

/*
 * task_deallocate
 */
bool task_deallocate(task_t task)
{
	if (task == 0 || task->ref_count == 0U) return false;

	task->ref_count--;
	return true;
}

/*
 * task_activate_address_space
 */
bool task_activate_address_space(task_t task)
{
	if (
		task == 0 ||
		!task->active ||
		task_is_kernel(task)
	) {
		return false;
	}

	return vm_address_space_activate(&task->map);
}

/*
 * task_terminate
 *
 * Terminate every thread owned by the task before marking the task itself
 * terminated.
 *
 * Thread resource reclamation is separate from termination because the
 * currently-executing thread cannot free the kernel stack it is using.
 */
bool task_terminate(task_t task)
{
	if (
		task == 0 ||
		!task->active ||
		task->state == TASK_STATE_TERMINATED
	) {
		return false;
	}

	task->halting = true;
	task->state = TASK_STATE_HALTING;

	for (;;) {
		thread_t thread = task_first_thread_ref(task);

		if (thread == 0) break;

		bool terminated = sched_is_initialized()
			? sched_thread_terminate(thread)
			: thread_terminate(thread);

		if (!terminated) {
			thread_deallocate(thread);

			task->halting = false;
			task->state = TASK_STATE_ACTIVE;

			return false;
		}

		/*
		 * Reaping may legitimately fail for the current thread because its
		 * kernel stack cannot be reclaimed until another thread is running.
		 */
		thread_reap(thread);
		thread_deallocate(thread);
	}

	/* Every thread of this task is gone by this point, so there is no
	 * concurrent access left to guard against; reclaim its mmap'd memory
	 * before the address space itself goes away. */
	vm_map_destroy_all(&task->map);

	task->active = false;
	task->halting = false;
	task->state = TASK_STATE_TERMINATED;

	return true;
}

/*
 * task_is_active
 */
bool task_is_active(task_t task)
{
	return task != 0 && task->active;
}

/*
 * task_is_kernel
 */
bool task_is_kernel(task_t task)
{
	return (
		task != 0 &&
		(task->flags & TASK_FLAG_KERNEL) != 0U
	);
}

/*
 * task_map
 */
vm_address_space_t *task_map(task_t task)
{
	if (task == 0) return 0;
	return &task->map;
}

/*
 * task_get_proc
 */
struct proc *task_get_proc(task_t task)
{
	if (task == 0) return 0;
	return task->bsd_info;
}

/*
 * task_user_entry
 *
 * Transitional access to the first thread's EL0 entry address.
 */
uint64_t task_user_entry(task_t task)
{
	if (task == 0) return 0ULL;

	thread_t thread = task_first_thread_ref(task);

	if (thread == 0) return 0ULL;

	uint64_t entry = thread_user_entry(thread);

	thread_deallocate(thread);
	return entry;
}

/*
 * task_user_stack
 *
 * Transitional access to the first thread's EL0 stack pointer.
 */
uint64_t task_user_stack(task_t task)
{
	if (task == 0) return 0ULL;

	thread_t thread = task_first_thread_ref(task);

	if (thread == 0) return 0ULL;

	uint64_t stack = thread_user_stack(thread);

	thread_deallocate(thread);
	return stack;
}
