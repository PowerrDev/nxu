#ifndef NXU_KERN_TASK_H
#define NXU_KERN_TASK_H

#include <vm/address_space.h>
#include <vm/vm_shm.h>

#include <stdbool.h>
#include <stdint.h>

struct proc;
struct thread;

/*
 * The most shared-memory regions one task may hold a handle reference to at
 * once (kern/ipc/shm_registry.h's shm_registry_attach): each nxu_shm_map
 * records its {region, va} pair here so a later nxu_shm_unmap can find the
 * region to release, and so task_terminate can release whatever the task
 * never explicitly unmapped. Small and fixed for the same reason
 * shm_next_va is a bump cursor rather than a real region list -- a handful
 * of regions per process is what this milestone needs.
 */
#define TASK_SHM_ATTACH_MAX 8U

typedef struct {
	vm_shm_region_t region;
	uint64_t va;
} task_shm_attachment_t;

typedef struct task *task_t;
typedef struct thread *thread_t;

typedef enum {
	TASK_STATE_INACTIVE,
	TASK_STATE_ACTIVE,
	TASK_STATE_HALTING,
	TASK_STATE_TERMINATED
} task_state_t;

typedef enum {
	TASK_FLAG_NONE = 0U,
	TASK_FLAG_KERNEL = 1U << 0U
} task_flag_t;

/*
 * struct task
 *
 * A task is the execution-resource container associated with a process.
 *
 * The task owns the virtual address space shared by its threads. CPU
 * execution state does not belong to the task itself: registers, user PC,
 * user SP, kernel stack and scheduler state belong to individual threads.
 *
 * threads
 *     Head of the intrusive thread list owned by this task.
 *
 * thread_count
 *     Number of threads currently attached to this task.
 *
 * active_thread_count
 *     Number of attached threads which remain active.
 *
 * bsd_info
 *     Back-pointer to the BSD-style proc object which owns this task.
 *
 * shm_next_va
 *     Bump cursor for vm_shm_map_into (vm/vm_shm.h), starting at
 *     VM_SHM_BASE. Not a real VMA/region list -- just enough to hand out a
 *     fresh, non-overlapping VA for each shared-memory region this task
 *     maps in turn.
 *
 * shm_attachments
 *     The handle references this task currently holds from nxu_shm_map,
 *     indexed by nothing in particular (first free slot on insert, linear
 *     scan on lookup) -- see TASK_SHM_ATTACH_MAX. Released by nxu_shm_unmap
 *     (which also clears its slot) or, for whatever is still here, by
 *     task_terminate.
 */
struct task {
	uint64_t task_uniqueid;

	uint32_t ref_count;
	task_state_t state;
	uint32_t flags;

	bool active;
	bool halting;

	vm_address_space_t map;
	uint64_t shm_next_va;
	task_shm_attachment_t shm_attachments[TASK_SHM_ATTACH_MAX];

	thread_t threads;
	uint32_t thread_count;
	uint32_t active_thread_count;

	uint32_t suspend_count;

	uint16_t priority;
	uint16_t max_priority;

	struct proc *bsd_info;
};

/*
 * Initialize the kernel task.
 */
bool task_init_kernel(
	task_t task,
	uint64_t uniqueid,
	struct proc *bsd_info
);

/*
 * Initialize a userspace task and create its initial thread.
 *
 * entry and stack are assigned to that initial thread rather than stored
 * directly in struct task.
 */
bool task_init_user(
	task_t task,
	uint64_t uniqueid,
	struct proc *bsd_info,
	uint64_t entry,
	uint64_t stack
);

/*
 * Initialize a userspace task as a copy of parent, for fork: a copy-on-write
 * copy of its address space and region list, and a first thread that resumes
 * where the calling thread's current system call returns, with 0 in the
 * result register. Must be called by a thread of parent while it is inside
 * the kernel for a system call.
 */
bool task_fork(
	task_t task,
	task_t parent,
	uint64_t uniqueid,
	struct proc *bsd_info
);

/*
 * Task reference management.
 */
bool task_reference(task_t task);
bool task_deallocate(task_t task);

/*
 * Activate the task's userspace address space.
 */
bool task_activate_address_space(task_t task);

/*
 * Terminate the task and all threads attached to it.
 */
bool task_terminate(task_t task);

/*
 * Task state inspection.
 */
bool task_is_active(task_t task);
bool task_is_kernel(task_t task);

/*
 * Task resource accessors.
 */
vm_address_space_t *task_map(task_t task);
struct proc *task_get_proc(task_t task);

/*
 * Transitional bootstrap accessors.
 *
 * User PC and SP now belong to the task's initial thread. These helpers
 * remain temporarily so the current EL0 bootstrap path does not need to
 * change before scheduler integration.
 */
uint64_t task_user_entry(task_t task);
uint64_t task_user_stack(task_t task);

#endif
