#ifndef NXU_KERN_THREAD_H
#define NXU_KERN_THREAD_H

#include <kern/cpuset.h>
#include <kern/lock.h>
#include <kern/machine/thread.h>

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

/*
 * A kernel thread that may be preempted where an interrupt arrives, not only at
 * the scheduler's own switch points. Only for code that takes no lock without
 * masking interrupts and holds no CPU-local state across a possible switch:
 * the scheduler treats an interrupted preemptible thread like an interrupted
 * EL0 one.
 */
#define TH_FLAG_PREEMPTIBLE 0x00000008U

struct task;
struct processor;
struct thread;
struct run_queue;
struct waitq;

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

	/*
	 * SMP scheduler state (rules in kern/sched_prism/sched.c).
	 *
	 * on_cpu is the CPU whose stack this thread's context is live on, from the
	 * moment that CPU chooses it until the CPU has switched away from it and is
	 * off its stack. While it is set the thread may be woken but must not be
	 * queued anywhere, or a second CPU could resume a context the first is still
	 * running on; a wakeup that arrives in that window is recorded in
	 * wakeup_deferred and acted on by the CPU that is switching away. Both are
	 * protected by sched_lock, a leaf lock taken with interrupts masked.
	 */
	nxu_spinlock_t sched_lock;
	struct processor *on_cpu;
	bool wakeup_deferred;

	/* The CPUs this thread may run on (thread_set_affinity), and the one it last ran on. */
	nxu_cpuset_t affinity;
	uint32_t last_cpu;

	/*
	 * Nesting count of "this thread is inside kernel code that assumes one CPU"
	 * (sched_bind_boot_cpu): while non-zero the scheduler keeps it on the boot
	 * CPU. Written only by the thread itself, read by any CPU placing it.
	 */
	uint32_t legacy_depth;

	/*
	 * The task this thread was created in, kept until the thread is reaped
	 * (`task` is cleared when it terminates). A thread that has been killed but
	 * is still finishing a system call on some CPU still needs to know which
	 * process it belongs to: current_proc() reads this, not `task`.
	 */
	task_t home_task;

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

	/*
	 * Multilevel feedback queue level. 0 is the highest-priority queue.
	 * The MLFQ policy in kern/sched_prism/sched.c keeps this in lockstep
	 * with sched_pri/base_pri; it exists as its own field because the
	 * mapping from level to priority is a policy detail the run queue
	 * itself does not need to know about.
	 */
	uint8_t mlfq_level;

	/*
	 * Timer ticks this thread has run at its current level since it last
	 * blocked or changed level. Unlike quantum_remaining, dispatching or
	 * yielding does not refill it, so a thread that spins in sched_yield()
	 * still runs out of its allotment and sinks. See sched_tick().
	 */
	uint32_t mlfq_ticks;

	uint32_t suspend_count;
	uint32_t quantum_remaining;

	/*
	 * Signals this thread will not take (bit n set = signal n blocked). A
	 * signal is process-directed -- pending on the proc -- and is delivered
	 * by whichever of its threads next returns to user mode without blocking
	 * it. See kern/process/signal.h.
	 */
	uint32_t sig_blocked;

	/*
	 * The wait queue this thread is sleeping on, or 0. wait_interruptible
	 * says a signal may cut the wait short; wait_interrupted is how it tells
	 * the sleeper it did. See kern/sched_prism/waitq.h.
	 */
	struct waitq *wait_queue;
	bool wait_interruptible;
	bool wait_interrupted;

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
 * Create an initial suspended userspace thread attached to task. arg becomes
 * the new thread's initial argument register (x0 on arm64, eax on i386; 0
 * for a process's own initial thread, a caller-supplied value for a
 * pthread-create-style spawned thread).
 *
 * The task maintains a residency reference while the thread remains
 * attached. The returned thread also carries a caller reference.
 */
bool thread_create(
	task_t task,
	uint64_t entry,
	uint64_t stack,
	uint64_t arg,
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
 * thread_create_cpu_idle
 *
 * Like thread_create_bootstrap, for a secondary CPU: the idle thread of a CPU
 * that idles on the stack it booted on. It is idle (never queued), pinned to
 * the given CPU and already running there.
 */
bool thread_create_cpu_idle(task_t task, uint32_t cpu, thread_t *result);

/*
 * thread_set_affinity / thread_get_affinity
 *
 * The set of CPUs a thread may run on. An empty set, or one with no CPU the
 * scheduler can use, is refused. Changing it takes effect at the thread's next
 * placement; a thread already on a disallowed CPU is moved when it next leaves
 * that CPU. Idle threads are pinned and cannot be changed.
 */
bool thread_set_affinity(thread_t thread, const nxu_cpuset_t *affinity);
bool thread_get_affinity(thread_t thread, nxu_cpuset_t *affinity);
bool thread_set_preemptible(thread_t thread, bool preemptible);
bool thread_is_preemptible(thread_t thread);

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
 * The current thread is the executing CPU's active_thread (struct processor).
 * It is only stable while the caller cannot migrate; see current_processor().
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

/* Thread-safe read of task->active_thread_count (kern/process/task.h) --
 * see the definition for why the raw field is not safe to read directly
 * once a task can have more than one thread. */
uint32_t task_active_thread_count(task_t task);

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
	uint64_t stack,
	uint64_t arg
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
