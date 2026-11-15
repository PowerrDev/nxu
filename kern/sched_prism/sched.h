/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/sched_prism/sched.h
 *
 * Declares the kernel sched_prism sched interfaces and data structures.
 */
#ifndef NXU_KERN_SCHED_H
#define NXU_KERN_SCHED_H

#include <kern/sched_prism/processor.h>
#include <kern/process/task.h>
#include <kern/process/thread.h>

#include <stdbool.h>
#include <stdint.h>

#define SCHED_DEFAULT_QUANTUM_TICKS 4U

/*
 * Multilevel feedback queue policy.
 *
 * MLFQ is implemented on top of the existing fixed-priority run queue: each
 * feedback level maps to one fixed sched_pri bucket, so queue selection and
 * FIFO round-robin sharing within a level come for free from run_queue_t.
 * The policy layer only decides which level a thread belongs to and how
 * long its quantum is at that level.
 *
 * Level 0 is the highest-priority queue. A thread starts at level 0 (the
 * "newbie premium"), drops one level whenever it exhausts a full quantum
 * without blocking (it is treated as CPU-bound), and every threads in the
 * system are boosted back to level 0 on a fixed interval so long-running
 * work cannot starve.
 */
#define SCHED_MLFQ_LEVELS 4U
#define SCHED_MLFQ_TOP_LEVEL 0U
#define SCHED_MLFQ_BOTTOM_LEVEL (SCHED_MLFQ_LEVELS - 1U)

/* ~4 seconds of SCHED_TICK_US (kern/sched_prism/run_queue.h) ticks. */
#define SCHED_MLFQ_BOOST_INTERVAL_TICKS 400U

typedef enum { SCHED_HEADQ, SCHED_TAILQ } sched_queue_placement_t;

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
 * sched_cpu_prepare
 *
 * Boot CPU: give a registered secondary CPU its idle thread before starting it.
 *
 * sched_cpu_idle
 *
 * Secondary CPU: the last thing its start-up does. Marks the CPU online for
 * scheduling and idles on the boot stack until work is queued on it.
 */
bool sched_cpu_prepare(uint32_t cpu);
__attribute__((noreturn)) void sched_cpu_idle(void);

/*
 * sched_bind_boot_cpu / sched_unbind_boot_cpu
 *
 * Bracket a call into kernel code that assumes a single CPU (VFS, filesystems,
 * drivers, process creation): the calling thread moves to the boot CPU and stays
 * there until the matching unbind. Nests. Hold no spinlock across the bind.
 * See kern/sched_prism/sched.c and doc/kern/smp.md.
 */
void sched_bind_boot_cpu(void);
void sched_unbind_boot_cpu(void);

/*
 * sched_thread_can_run_on
 *
 * Whether the scheduler would ever place `thread` on `cpu`: its affinity
 * allows it, or it is inside single-CPU kernel code (sched_bind_boot_cpu) and
 * `cpu` is the boot CPU. thread_can_run_on_cpu() in the design notes.
 */
bool sched_thread_can_run_on(thread_t thread, uint32_t cpu);

/*
 * sched_kick_thread / sched_wait_thread_off_cpu
 *
 * For tearing a thread down that may be running on another CPU: interrupt that
 * CPU so the thread notices its new state, and wait until the thread is off it.
 */
void sched_kick_thread(thread_t thread);
bool sched_wait_thread_off_cpu(thread_t thread);

/*
 * thread_setrun
 *
 * Make one runnable non-idle thread eligible to run: choose a CPU for it (its
 * affinity, where it last ran, how loaded the CPUs are) and queue it there. A
 * thread that is still switching out is queued by the CPU that is switching
 * away, once its context is saved. Safe from any CPU and from interrupt context.
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
 * sched_block_commit
 *
 * The second half of sched_block, for a caller that has already moved the
 * current thread to the waiting state itself (sched_thread_wait) under a lock
 * that its waker also takes, so the wakeup cannot slip in between (waitq_block).
 * Interrupts must stay masked from sched_thread_wait until this returns: an
 * interrupt in between could switch away from a thread already marked waiting.
 */
bool sched_block_commit(void);

/*
 * sched_sleep_us
 *
 * Put the calling thread to sleep for at least `microseconds` (rounded up to the
 * boot CPU's next timer tick, 10 ms). The thread is off every run queue while it
 * sleeps, so it does not count as load: a harness that waits with this instead of
 * a sched_yield() loop leaves its CPU free for the work it is waiting for.
 * Returns false if it could not sleep (no scheduler, idle thread).
 */
bool sched_sleep_us(uint64_t microseconds);

/*
 * sched_sleep_us_interruptible
 *
 * sched_sleep_us that a signal can end early, for a sleep a user program asked
 * for: a signal that is not blocked or ignored ends it with *interrupted set.
 * Returns false, as sched_sleep_us does, if the thread could not sleep.
 */
bool sched_sleep_us_interruptible(uint64_t microseconds, bool *interrupted);

/*
 * True once the boot CPU has taken a scheduler tick. Timed sleeps are ended by
 * the tick, so before this they wait by yielding instead.
 */
bool sched_is_ticking(void);

/*
 * sched_set_ticking
 *
 * Timed waits can end from now on: the event timer has started delivering
 * timer calls (kern/sched_prism/sched_timer.c). The first scheduler tick on
 * the boot CPU says the same with a periodic event timer.
 */
void sched_set_ticking(void);

/*
 * sched_timer_init / sched_timer_enabled / sched_timer_dispatch
 *
 * The scheduler's deadlines on a deadline event timer (see
 * kern/sched_prism/sched_timer.c): a tick every SCHED_TICK_US while a CPU
 * runs a thread, none while it is idle. sched_timer_init connects them, once,
 * on the boot CPU, after etimer_init (it does nothing with a periodic event
 * timer, whose tick keeps driving the scheduler). sched_timer_dispatch is
 * sched_switch's: this CPU is switching to its idle thread (`idle_next`) or
 * to a thread. Interrupts masked.
 */
void sched_timer_init(void);
bool sched_timer_enabled(void);
void sched_timer_dispatch(processor_t processor, bool idle_next, uint64_t now);

/*
 * sched_deadline_tick
 *
 * With a periodic event timer: runs the timer calls that are due (timed
 * waits among them) without charging the scheduler a tick. For a periodic
 * interrupt that comes more often than the scheduler ticks (i386's PIT when
 * the Local APIC timer cannot be used), so timer calls run to the
 * interrupt's period rather than the tick's. Boot CPU, interrupt context.
 */
void sched_deadline_tick(void);

/*
 * sched_sleep_until_kicked / sched_kick_sleepers
 *
 * sched_sleep_us that also ends at the next sched_kick_sleepers(): for a loop
 * that must notice work the moment it arrives (the desktop: input from an
 * interrupt, a frame from an app) but otherwise sleeps. Returns false if it
 * could not sleep. Kicking is safe from interrupt handlers.
 */
uint32_t sched_kick_sequence(void);
bool sched_sleep_until_kicked_seq(uint64_t microseconds, uint32_t kicks, bool interruptible);
bool sched_sleep_until_kicked(uint64_t microseconds);
void sched_kick_sleepers(void);

/*
 * sched_exit_current
 *
 * Switch away from a thread which has already entered TH_TERMINATE. This
 * routine never returns on the terminating thread's stack.
 */
__attribute__((noreturn)) void sched_exit_current(void);

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
 * sched_mlfq_level_priority
 *
 * The fixed run-queue priority a given MLFQ level is scheduled at. Levels
 * above SCHED_MLFQ_BOTTOM_LEVEL are clamped to the bottom queue.
 */
uint16_t sched_mlfq_level_priority(uint8_t level);

/*
 * sched_thread_level_priority
 *
 * The run-queue priority `thread` is scheduled at on MLFQ level `level`:
 * the level's place in the thread's QoS band (sched_mlfq_level_priority is
 * the DEFAULT band's).
 */
uint16_t sched_thread_level_priority(thread_t thread, uint8_t level);

/*
 * sched_mlfq_level_quantum
 *
 * Quantum length, in timer ticks, granted to a thread at a given MLFQ
 * level. Deeper levels get longer quanta so CPU-bound work is preempted
 * less often once it has been identified as such.
 */
uint32_t sched_mlfq_level_quantum(uint8_t level);

/*
 * sched_task_set_qos
 *
 * Put every thread of `task` in quality-of-service band `qos` (NXU_QOS_*):
 * each takes it the next time it is queued or changes level. The caller
 * decides who may (see syscall_set_qos).
 */
bool sched_task_set_qos(task_t task, uint8_t qos);

/*
 * sched_task_effective_qos
 *
 * The QoS `task`'s threads run at: its own, or one donated to it over NXPC
 * while that is higher and has not lapsed.
 *
 * sched_task_qos_donate
 *
 * A process at `qos` sent `task` a message: `task` runs at least at `qos` for
 * a short while (renewed by every such message), so a service a user-facing
 * process is waiting on is not left behind lower-QoS work (XNU's QoS
 * importance donation).
 *
 * sched_task_qos_handling
 *
 * `task` took a message from a sender at `qos` (NXU_QOS_COUNT: it found none):
 * it runs at that QoS while it handles it, and at its own once its queue is
 * empty.
 */
uint8_t sched_task_effective_qos(task_t task);
void sched_task_qos_donate(task_t task, uint8_t qos);
void sched_task_qos_handling(task_t task, uint8_t qos);

/* A QoS class's name, for logs. */
const char *sched_qos_name(uint8_t qos);

/*
 * sched_run_queue_self_test
 */
bool sched_run_queue_self_test(void);

/*
 * sched_mlfq_self_test
 */
bool sched_mlfq_self_test(void);

/*
 * sched_validate
 */
bool sched_validate(void);

/*
 * The most CPUs that have been executing threads of user processes at the same
 * time since the peak was last reset (a user thread counts while it is on a CPU,
 * in user mode or inside a system call). For the SMP tests.
 */
uint32_t sched_user_running_peak(void);
void sched_user_running_reset_peak(void);

/* The cross-CPU run queue invariants (see sched.c); walks every queue, for tests. */
bool sched_validate_all(void);

/*
 * sched_dump
 */
void sched_dump(void);

#endif
