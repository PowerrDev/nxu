#ifndef NXU_KERN_SCHED_H
#define NXU_KERN_SCHED_H

#include <kern/sched_prism/processor.h>
#include <kern/process/task.h>
#include <kern/process/thread.h>

#include <stdbool.h>
#include <stdint.h>

#define SCHED_DEFAULT_QUANTUM_TICKS 4U

typedef enum {
	SCHED_HEADQ,
	SCHED_TAILQ
} sched_queue_placement_t;

/*
 * sched_bootstrap
 *
 * Initialize the boot processor, represent the existing boot context as a
 * kernel thread and create the processor idle thread.
 */
bool sched_bootstrap(task_t kernel_task);

/*
 * sched_is_initialized
 */
bool sched_is_initialized(void);

/*
 * sched_bootstrap_thread
 */
thread_t sched_bootstrap_thread(void);

/*
 * sched_idle_thread
 */
thread_t sched_idle_thread(void);

/*
 * thread_setrun
 *
 * Queue one runnable non-idle thread on the current processor run queue.
 */
bool thread_setrun(thread_t thread, sched_queue_placement_t placement);

/*
 * thread_run_queue_remove
 */
bool thread_run_queue_remove(thread_t thread);

/*
 * thread_select
 *
 * Remove the highest-priority runnable thread, falling back to the processor
 * idle thread when no normal thread is available.
 */
thread_t thread_select(processor_t processor);

/*
 * sched_thread_start
 *
 * Prepare a never-run thread's machine context, release its creation hold
 * and enqueue it when appropriate.
 */
bool sched_thread_start(thread_t thread);

/*
 * Scheduler-aware thread state transitions.
 */
bool sched_thread_wait(thread_t thread, bool uninterruptible);
bool sched_thread_wakeup(thread_t thread);
bool sched_thread_hold(thread_t thread);
bool sched_thread_release(thread_t thread);
bool sched_thread_terminate(thread_t thread);
bool sched_thread_set_priority(thread_t thread, uint16_t priority);

/*
 * sched_yield
 *
 * Voluntarily give another runnable thread an opportunity to execute. The
 * caller returns when the scheduler later selects it again.
 */
bool sched_yield(void);

/*
 * sched_block
 *
 * Block the current thread and dispatch another thread. The call returns only
 * after sched_thread_wakeup() makes the thread runnable and it is selected.
 */
bool sched_block(bool uninterruptible);

/*
 * sched_exit_current
 *
 * Switch away from a thread which has already entered TH_TERMINATE. This
 * routine never returns on the terminating thread's stack.
 */
__attribute__((noreturn))
void sched_exit_current(void);

/*
 * sched_tick
 *
 * Charge one timer tick to the active thread and request preemption when its
 * quantum expires.
 */
void sched_tick(void);

/*
 * sched_preempt
 *
 * Service a deferred preemption request. Callers must only invoke this from
 * a scheduler-safe exception context, currently EL0 IRQ entry or the idle
 * thread's IRQ path.
 */
bool sched_preempt(void);

bool sched_preemption_pending(void);
void sched_clear_preemption(void);

/*
 * sched_run_queue_self_test
 */
bool sched_run_queue_self_test(void);

/*
 * sched_validate
 */
bool sched_validate(void);

/*
 * sched_dump
 */
void sched_dump(void);

#endif
