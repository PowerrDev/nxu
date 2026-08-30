#ifndef NXU_KERN_PROCESSOR_H
#define NXU_KERN_PROCESSOR_H

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
 * Scheduler-visible state for one logical CPU.
 *
 * NXU currently boots one AArch64 processor. Keeping this state in a
 * processor object now prevents current-thread and run-queue globals from
 * becoming an SMP migration problem later.
 */
struct processor {
	uint32_t cpu_id;
	processor_state_t state;

	thread_t active_thread;
	thread_t idle_thread;
	thread_t next_thread;
	thread_t previous_thread;

	run_queue_t runq;

	uint64_t dispatch_count;
	uint64_t context_switch_count;
	uint64_t preemption_count;
	uint64_t quantum_expiration_count;

	bool preemption_pending;
};

/*
 * processor_bootstrap
 *
 * Initialize CPU 0 and its local run queue.
 */
bool processor_bootstrap(void);

/*
 * current_processor
 *
 * Return the processor associated with the executing CPU. This is the boot
 * processor until MPIDR-based per-CPU lookup is introduced.
 */
processor_t current_processor(void);

/*
 * processor_validate
 */
bool processor_validate(processor_t processor);

#endif
