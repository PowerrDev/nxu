#include <kern/process/task.h>
#include <kern/console/console.h>
#include <kern/sched_prism/sched.h>
#include <kern/process/thread.h>
#include <kern/machine/user.h>
#include <vm/pmm.h>
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
 * task_fork
 *
 * Initialize a userspace task as a copy of parent. See task.h.
 */
bool task_fork(
	task_t task,
	task_t parent,
	uint64_t uniqueid,
	struct proc *bsd_info
)
{
	if (
		task == 0 ||
		parent == 0 ||
		bsd_info == 0 ||
		task_is_kernel(parent) ||
		!task_is_active(parent)
	) {
		return false;
	}

	thread_t caller = current_thread();

	if (caller == 0 || caller->task != parent) return false;

	task_reset(task);

	if (!vm_address_space_fork(&parent->map, &task->map)) return false;

	if (!vm_map_fork(&parent->map, &task->map)) {
		(void)vm_address_space_release_pages(&task->map);
		(void)vm_address_space_destroy(&task->map);
		return false;
	}

	task->shm_next_va = parent->shm_next_va;
	task->task_uniqueid = uniqueid;
	task->ref_count = 1U;
	task->state = TASK_STATE_ACTIVE;
	task->flags = TASK_FLAG_NONE;
	task->active = true;
	task->bsd_info = bsd_info;

	/*
	 * thread_create wants a user entry point and stack; the child never
	 * runs at either -- machine_user_fork_state replaces them, and every
	 * register, with the parent's. Any non-zero pair will do.
	 */
	thread_t initial_thread;

	if (!thread_create(task, PMM_PAGE_SIZE, PMM_PAGE_SIZE, 0ULL, &initial_thread)) {
		goto fail_address_space;
	}

	if (!machine_user_fork_state(&initial_thread->machine)) {
		(void)thread_terminate(initial_thread);
		(void)thread_reap(initial_thread);
		thread_deallocate(initial_thread);
		goto fail_address_space;
	}

	initial_thread->sig_blocked = caller->sig_blocked;

	/* Task membership keeps the thread resident once this reference goes. */
	thread_deallocate(initial_thread);

	return true;

fail_address_space:
	vm_map_destroy_all(&task->map);
	(void)vm_address_space_release_pages(&task->map);
	(void)vm_address_space_destroy(&task->map);

	task->active = false;
	task->state = TASK_STATE_TERMINATED;
	return false;
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
 * task_release_shm_attachments
 *
 * Releases the handle reference (kern/ipc/shm_registry.h) behind every
 * shared-memory mapping the task never explicitly gave up with
 * nxu_shm_unmap, and clears the table. The mapping's own PMM reference is
 * unaffected here -- it is dropped generically for every mapped page,
 * shared-memory or not, by vm_address_space_release_pages; this only drops
 * the separate handle reference syscall_shm_map's shm_registry_attach took
 * out (see vm/vm_shm.h's usage protocol: the two are independent).
 *
 * Callers must have already established that no thread of this task can
 * still be executing (task_terminate's all_off_cpu): a thread mid-syscall in
 * syscall_shm_map/_unmap touches this same table.
 */
static void
task_release_shm_attachments(task_t task)
{
	for (uint32_t index = 0U; index < TASK_SHM_ATTACH_MAX; index++) {
		if (task->shm_attachments[index].region == VM_SHM_REGION_NULL) continue;

		vm_shm_release(task->shm_attachments[index].region);
		task->shm_attachments[index].region = VM_SHM_REGION_NULL;
		task->shm_attachments[index].va = 0ULL;
	}
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

	thread_t self = current_thread();
	bool all_off_cpu = true;

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
		 * The thread is terminated but may still be running: on another CPU, in
		 * user mode or in the middle of a system call. Nothing of the task can
		 * be freed while that is so, so wait for it to be off its CPU (it is
		 * interrupted to notice). A thread that is asleep, queued or never ran is
		 * off already. The calling thread is excluded (it is the one running this,
		 * and leaves by returning): it is still on its CPU by definition.
		 */
		if (thread != self && sched_is_initialized() && !sched_wait_thread_off_cpu(thread)) {
			all_off_cpu = false;
			kprintf("task_terminate: a thread of the task would not stop; its memory is leaked\n");
		}

		/*
		 * Reaping may legitimately fail for the current thread because its
		 * kernel stack cannot be reclaimed until another thread is running.
		 */
		if (all_off_cpu) thread_reap(thread);
		thread_deallocate(thread);
	}

	/*
	 * Every thread of this task has been terminated and is off its CPU, so
	 * nothing will touch its memory -- or task->shm_attachments -- again.
	 * Give back whatever shared-memory handle references this task itself
	 * still held (its mappings' own PMM references come back below,
	 * regardless of this).
	 */
	if (all_off_cpu && !task_is_kernel(task)) task_release_shm_attachments(task);

	/*
	 * ... except that a CPU may still have the address space loaded in TTBR0
	 * until it has switched away for the last time. Wait for that too (the
	 * calling CPU leaves it here), then reclaim.
	 */
	bool memory_free = all_off_cpu && (task_is_kernel(task) || vm_address_space_quiesce(&task->map));

	if (memory_free) {
		vm_map_destroy_all(&task->map);

		/* Give back every remaining page (the image, the stack, shared
		 * mappings' references) and then the page tables themselves. */
		if (!task_is_kernel(task)) {
			(void)vm_address_space_release_pages(&task->map);
			(void)vm_address_space_destroy(&task->map);
		}
	} else {
		kprintf("task_terminate: address space not released (a CPU still uses it): leaked\n");
	}

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
