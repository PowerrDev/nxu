#ifndef NXU_KERN_PROCESSOR_H
#define NXU_KERN_PROCESSOR_H

#include <kern/cpuset.h>
#include <kern/lock.h>
#include <kern/sched_prism/run_queue.h>
#include <kern/process/thread.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct processor *processor_t;

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
 * processor_validate
 */
bool processor_validate(processor_t processor);

#endif
