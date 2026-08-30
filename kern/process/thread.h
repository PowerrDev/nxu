#ifndef NXU_KERN_THREAD_H
#define NXU_KERN_THREAD_H

#include <arch/arm64/thread.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define THREAD_MAX 256U
#define THREAD_ID_INVALID UINT64_MAX

#define THREAD_KERNEL_STACK_SIZE 16384ULL

#define THREAD_PRIORITY_MIN 0U
#define THREAD_PRIORITY_DEFAULT 31U
#define THREAD_PRIORITY_MAX 63U

/*
 * Thread state bits.
 *
 * Thread scheduling state is represented as a collection of state bits,
 * allowing conditions such as suspension and waiting to coexist.
 */
#define TH_WAIT 0x01U
#define TH_SUSP 0x02U
#define TH_RUN 0x04U
#define TH_UNINT 0x08U
#define TH_TERMINATE 0x10U
#define TH_IDLE 0x80U
#define TH_WAKING 0x100U

/*
 * NXU-private thread flags.
 */
#define TH_FLAG_NONE 0x00000000U
#define TH_FLAG_KERNEL 0x00000001U
#define TH_FLAG_REAPED 0x00000002U
#define TH_FLAG_BOOTSTRAP 0x00000004U

struct task;
struct thread;
struct run_queue;

typedef struct task *task_t;
typedef struct thread *thread_t;

typedef uint64_t thread_id_t;
typedef void (*thread_continue_t)(void *parameter);

/*
 * Scheduler linkage.
 *
 * A thread uses this storage for either runnable-queue membership or
 * wait-queue membership. Task and global thread lists have independent
 * linkage because a thread may belong to those lists simultaneously.
 */
typedef union {
	struct {
		thread_t prev;
		thread_t next;
	} runq;

	struct {
		thread_t prev;
		thread_t next;
	} waitq;
} thread_sched_links_t;

/*
 * struct thread
 *
 * Fundamental schedulable execution object.
 *
 * A task owns an address space and a collection of threads. Each thread
 * owns the execution state necessary for one independent flow of control.
 *
 * A thread can simultaneously belong to:
 *
 *     threads_prev / threads_next
 *         Global thread list.
 *
 *     task_prev / task_next
 *         Owning task's thread list.
 *
 *     sched_links
 *         Scheduler run queue or wait queue.
 *
 * The scheduler is implemented separately. Scheduler-visible state is kept
 * here so introducing the scheduler does not require restructuring thread
 * objects later.
 */
struct thread {
	thread_t threads_prev;
	thread_t threads_next;

	thread_t task_prev;
	thread_t task_next;

	thread_sched_links_t sched_links;
	struct run_queue *runq;

	task_t task;
	thread_id_t thread_id;

	uint32_t ref_count;
	uint32_t state;
	uint32_t flags;

	bool active;
	bool started;

	uint16_t sched_pri;
	uint16_t base_pri;
	uint16_t max_priority;

	uint32_t suspend_count;
	uint32_t quantum_remaining;

	thread_continue_t continuation;
	void *parameter;

	void *kernel_stack;
	uint64_t kernel_stack_size;

	machine_thread_t machine;

	uint32_t slot;
};

/*
 * thread_bootstrap
 *
 * Initialize global thread management state.
 */
bool thread_bootstrap(void);

/*
 * thread_create
 *
 * Create an initial suspended userspace thread attached to task.
 *
 * The task maintains a residency reference while the thread remains
 * attached. The returned thread also carries a caller reference.
 */
bool thread_create(
	task_t task,
	uint64_t entry,
	uint64_t stack,
	thread_t *result
);

/*
 * kernel_thread_create
 *
 * Create an initial suspended kernel thread attached to task.
 *
 * continuation becomes the thread's initial C entry point once scheduler
 * bootstrap and machine context startup are implemented.
 */
bool kernel_thread_create(
	task_t task,
	thread_continue_t continuation,
	void *parameter,
	thread_t *result
);

/*
 * thread_create_bootstrap
 *
 * Represent the kernel execution context which existed before scheduler
 * startup. It uses the boot stack already active in SP_EL1 and therefore
 * does not allocate a new stack.
 */
bool thread_create_bootstrap(task_t task, thread_t *result);

/*
 * Thread reference management.
 */
bool thread_reference(thread_t thread);
void thread_deallocate(thread_t thread);

/*
 * thread_start
 *
 * Release the creation suspension and make the thread runnable.
 */
bool thread_start(thread_t thread);

/*
 * Waiting and wakeup state transitions.
 */
bool thread_wait(thread_t thread, bool uninterruptible);
bool thread_go(thread_t thread);

/*
 * Suspension management.
 */
bool thread_hold(thread_t thread);
bool thread_release(thread_t thread);

/*
 * thread_terminate
 *
 * Remove a thread from execution eligibility and detach it from its task.
 *
 * Machine resources which cannot safely be destroyed while executing,
 * particularly the current kernel stack, remain until thread_reap().
 */
bool thread_terminate(thread_t thread);

/*
 * thread_reap
 *
 * Reclaim termination resources once the thread is no longer executing.
 */
bool thread_reap(thread_t thread);

/*
 * Kernel stack management.
 */
bool thread_stack_alloc(thread_t thread);
bool thread_stack_free(thread_t thread);

/*
 * Current thread access.
 *
 * This is global while NXU is single-processor. The scheduler will later
 * move current-thread ownership into processor-local state.
 */
bool thread_set_current(thread_t thread);
thread_t current_thread(void);

/*
 * Task thread traversal.
 *
 * Returned thread objects carry references which must be released using
 * thread_deallocate().
 */
thread_t task_first_thread_ref(task_t task);
thread_t thread_next_task_thread_ref(thread_t thread);

/*
 * Thread identity and ownership.
 */
thread_id_t thread_tid(thread_t thread);
task_t thread_task(thread_t thread);

/*
 * Thread state inspection.
 */
bool thread_is_active(thread_t thread);
bool thread_is_started(thread_t thread);
bool thread_is_runnable(thread_t thread);
bool thread_is_waiting(thread_t thread);
bool thread_is_suspended(thread_t thread);
bool thread_is_terminated(thread_t thread);
bool thread_is_idle(thread_t thread);
bool thread_is_bootstrap(thread_t thread);

/*
 * Scheduling attributes.
 */
bool thread_set_idle(thread_t thread);
bool thread_set_priority(thread_t thread, uint16_t priority);
uint16_t thread_priority(thread_t thread);
uint16_t thread_base_priority(thread_t thread);

/*
 * Userspace machine state.
 */
uint64_t thread_user_entry(thread_t thread);
uint64_t thread_user_stack(thread_t thread);

bool thread_set_user_state(
	thread_t thread,
	uint64_t entry,
	uint64_t stack
);

/*
 * Kernel stack inspection.
 */
uint64_t thread_kernel_stack_base(thread_t thread);
uint64_t thread_kernel_stack_top(thread_t thread);

/*
 * Thread subsystem inspection.
 */
uint32_t thread_count(void);
bool thread_validate(void);
void thread_dump(void);

#endif
