#ifndef NXU_KERN_PROCESSOR_H
#define NXU_KERN_PROCESSOR_H

#include <kern/cpuset.h>
#include <kern/lock.h>
#include <kern/sched_prism/run_queue.h>
#include <kern/process/thread.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct processor *processor_t;

struct vm_address_space;

typedef enum {
	PROCESSOR_OFFLINE,
	PROCESSOR_STARTING,
	PROCESSOR_RUNNING,
	PROCESSOR_IDLE,
	PROCESSOR_SHUTDOWN
} processor_state_t;

/*
 * struct processor
 *
 * Everything the kernel keeps per logical CPU: identity, the scheduler's
 * current/idle threads and MLFQ run queue, and the CPU's own counters. This is
 * NXU's per-CPU object; TPIDR_EL1 (see <kern/machine/smp.h>) points at it, so
 * current_processor() is one register read.
 *
 * Locking (the scheduler's rules are in kern/sched_prism/sched.c):
 *
 *   runq, runq_lock       runq_lock protects the run queue and the fields
 *                         marked (rq) below. Any CPU may take another CPU's
 *                         runq_lock to enqueue or migrate a thread onto it.
 *   fields marked (own)   written only by the owning CPU, with interrupts
 *                         masked; other CPUs may read them racily for load
 *                         estimates but must not rely on them for correctness.
 */
struct processor {
	/* MUST stay the first member: machine_cpu_id() reads the first word. */
	uint32_t cpu_id;
	processor_state_t state;

	/* The MPIDR affinity this CPU is known to firmware and the GIC by. */
	uint64_t mpidr;

	/*
	 * (own) The user address space loaded in this CPU's TTBR0, or 0 when TTBR0
	 * walks are disabled (idle, kernel threads). Each CPU owns its translation
	 * state; nothing else writes this. See vm/address_space.c.
	 */
	struct vm_address_space *active_space;

	thread_t active_thread;			/* (own) the thread running on this CPU */
	thread_t idle_thread;			/* never migrates, never on a run queue */
	thread_t next_thread;
	thread_t previous_thread;		/* (own) handed to sched_finish_switch */

	/*
	 * What sched_finish_switch does with previous_thread once its stack is no
	 * longer in use: put it back on this CPU's run queue (it yielded or was
	 * preempted), or leave it alone (it blocked, or exited). See sched.c.
	 */
	bool previous_requeue;

	run_queue_t runq;			/* (rq) */
	nxu_spinlock_t runq_lock;

	uint64_t dispatch_count;
	uint64_t context_switch_count;
	uint64_t preemption_count;
	uint64_t quantum_expiration_count;

	/* Timer interrupts this CPU has taken. */
	uint64_t ticks;

	/* (own) ticks since this CPU's run queue was last boosted. */
	uint32_t boost_ticks;

	uint64_t user_dispatch_count;		/* times this CPU switched to a thread of a user process */
	uint64_t ipi_reschedule_count;		/* reschedule IPIs taken */
	uint64_t migrate_in_count;		/* threads placed or moved here by another CPU */
	uint64_t migrate_out_count;		/* threads this CPU's balancer moved elsewhere */

	/*
	 * Set by the timer tick, by another CPU's reschedule IPI, or when a
	 * better thread is queued here: the next safe return point must enter the
	 * scheduler. Atomic (relaxed is enough for the flag; the run queue state
	 * it announces is published under runq_lock).
	 */
	bool preemption_pending;

	/* The boot stack this CPU came up on (0 for the boot CPU, which uses the static one). */
	void *boot_stack;
};

/*
 * processor_bootstrap
 *
 * Initialize CPU 0 and its local run queue, and make it the current CPU.
 */
bool processor_bootstrap(void);

/*
 * current_processor
 *
 * The processor object of the executing CPU. Before SMP bring-up (and on ports
 * with a single CPU) this is the boot processor.
 *
 * The result is only stable while the caller cannot migrate: threads are
 * never preempted inside the kernel except at the scheduler's own safe points,
 * and code that must be certain masks interrupts.
 */
processor_t current_processor(void);

/*
 * processor_register
 *
 * Create the processor object for logical CPU `cpu_id` (1 ..) with MPIDR
 * `mpidr`, in the OFFLINE state. Called by the boot CPU before it starts the
 * CPU. Returns 0 if `cpu_id` is out of range or already registered.
 */
processor_t processor_register(uint32_t cpu_id, uint64_t mpidr);

/* The processor for a logical CPU id, or 0 if none is registered. */
processor_t processor_by_id(uint32_t cpu_id);

/* One past the highest registered logical CPU id (registered, not necessarily online). */
uint32_t processor_count(void);

/* The CPUs currently able to run threads (state RUNNING or IDLE). */
void processor_online_set(nxu_cpuset_t *set);

/*
 * processor_default_affinity / processor_set_default_affinity
 *
 * The affinity a new thread starts with. It is the boot CPU alone until the
 * kernel subsystems threads run in have been made SMP-safe (see
 * doc/kern/smp.md); code that knows its threads are safe widens it (SMP tests,
 * the `sched.affinity=all` boot argument) or sets a thread's own with
 * thread_set_affinity().
 */
void processor_default_affinity(nxu_cpuset_t *affinity);
void processor_set_default_affinity(const nxu_cpuset_t *affinity);

/* The same for threads of user processes (see processor.c). */
void processor_default_user_affinity(nxu_cpuset_t *affinity);
void processor_set_default_user_affinity(const nxu_cpuset_t *affinity);

/*
 * processor_is_online
 *
 * Whether a CPU can take threads: it is running its idle loop or a thread.
 */
bool processor_is_online(const struct processor *processor);

/*
 * processor_load
 *
 * How many threads want this CPU: queued plus running (idle does not count).
 * Read without the run queue lock, so it is only an estimate; placement and
 * balancing decisions made from it are re-checked under the lock.
 */
uint32_t processor_load(const struct processor *processor);

/*
 * processor_validate
 */
bool processor_validate(processor_t processor);

#endif
