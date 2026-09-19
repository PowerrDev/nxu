# Scheduler

NXU uses a single-processor fixed-priority run queue with a multilevel feedback queue (MLFQ) policy layered on top. Threads are the schedulable objects, `processor_t` owns CPU-local scheduling state, and `run_queue_t` owns runnable-queue ordering.

Scheduling policy is deliberately separate from the AArch64 context-switch mechanism. Priority policy, queue selection, and quantum accounting can therefore change without replacing the thread machine context or the switch ABI.

## Current model

CPU 0 owns one `processor_t`:

```text
processor 0
├── active_thread
├── idle_thread
├── next_thread
├── previous_thread
└── runq
    └── 64 priority queues
```

Higher numeric scheduling priorities run before lower priorities. Threads at the same priority are queued FIFO when inserted at the tail.

The idle thread is not inserted in the normal run queue. It is selected only when no ordinary runnable thread exists.

## Multilevel feedback queue policy

`kern/sched_prism/sched.c` implements MLFQ on top of the fixed-priority run queue rather than beside it. Each feedback level is scheduled at exactly one fixed `sched_pri` bucket, so the run queue's existing "always dequeue the highest non-empty bucket, FIFO within a bucket" behavior already gives MLFQ its rank ordering and round-robin sharing for free. The policy layer only owns the level<->priority/quantum mapping and the rules for moving a thread between levels.

```text
level  sched_pri  quantum (ticks)
0 (top)       63                4
1             47                8
2             31               16
3 (bottom)    15               32
```

`SCHED_MLFQ_LEVELS`, the priority table and the quantum table live in `kern/sched_prism/sched.c`; `sched_mlfq_level_priority()` and `sched_mlfq_level_quantum()` expose the mapping. Quanta grow at deeper levels so CPU-bound work that has already been identified as such is preempted less often, which cuts context-switch overhead for background work without touching interactive responsiveness at level 0.

The five rules and where each lives:

1. **Rank.** `run_queue_dequeue()` always returns the thread in the highest non-empty priority bucket, so a runnable level-0 thread always preempts or is selected ahead of anything at level 1-3. Unchanged by MLFQ.
2. **Round-robin within a line.** Threads enqueued at `RUN_QUEUE_TAIL` at the same `sched_pri` are already FIFO. MLFQ pins every thread at a level to that level's one fixed priority, so this is inherited directly.
3. **Newbie premium.** `sched_thread_start()` calls `sched_mlfq_assign(thread, SCHED_MLFQ_TOP_LEVEL)` before the thread is ever enqueued, so a brand-new thread always starts at level 0.
4. **Demotion.** `sched_tick()` charges one tick against `thread->quantum_remaining`. If that reaches zero, the thread ran its entire slice without blocking or yielding (both of those paths dispatch through `sched_switch()` before the quantum hits zero and get a fresh quantum at the same level), so it is treated as CPU-bound: `sched_mlfq_assign()` drops it one level (clamped at `SCHED_MLFQ_BOTTOM_LEVEL`) and grants the new level's quantum. A thread that blocks for I/O before exhausting its quantum never gets demoted, which is what keeps interactive work at the top.
5. **Equalizer boost.** `sched_tick()` also advances a `SCHED_MLFQ_BOOST_INTERVAL_TICKS`-tick counter (400 ticks, ~4s at the 100 Hz kernel timer) independent of which thread is active. When it fires, `sched_mlfq_boost_locked()` drains every run-queue bucket below level 0 with `run_queue_dequeue_priority()`, reassigns each thread to level 0 with a fresh quantum, and does the same to the currently active thread. This runs even while the CPU is idle, so work that arrived and was demoted during a quiet period does not stay parked at the bottom indefinitely.

`sched_mlfq_self_test()` exercises the demotion ladder (including that it clamps at the bottom) and the boost path against synthetic threads and a throwaway `processor_t`, and runs at boot alongside `sched_run_queue_self_test()`.

A thread pinned to an explicit priority through `sched_thread_set_priority()` is not touched by demotion or boost logic directly - that call is an escape hatch outside the MLFQ ladder. It only rejoins normal MLFQ movement the next time `sched_mlfq_assign()` is called on it (thread start, demotion, or boost).

## Lesson 24 context switching

Lesson 24 completes the first real scheduler dispatch path.

### 24A — AArch64 kernel context

`arm64_kernel_context_t` preserves:

```text
x19-x28
x29 / fp
x30 / lr
SP_EL1
```

`arm64_switch_context()` is implemented in `mach/arm64/context_switch.S`. It saves the outgoing context, replaces the kernel stack pointer, restores the incoming context and returns through the incoming x30.

Caller-saved x0-x18 are not part of the ordinary scheduler context. Asynchronous exceptions preserve the full architectural state in `arm64_exception_frame_t` instead.

### 24B — first-run threads

A newly created thread has never called the scheduler and therefore has no return address to restore.

Before its first dispatch, `sched_thread_start()` builds a synthetic machine context:

```text
context.sp = top of the thread's kernel stack
context.lr = sched_thread_continue
```

The first `ret` performed by `arm64_switch_context()` enters `sched_thread_continue()` on the new stack.

Kernel threads invoke their stored continuation. User threads enter EL0 through `arm64_enter_el0()`.

### 24C — task address spaces

TTBR1 always describes the NXU kernel.

TTBR0 follows the task of the selected thread:

```text
kernel thread  -> TTBR0 disabled
user thread    -> task->map installed in TTBR0_EL1
```

`vm_address_space_deactivate()` marks the previous userspace map inactive and disables TTBR0 when the scheduler selects a kernel thread.

The currently selected BSD-style `proc` is changed at the same scheduler boundary.

### 24D — per-thread EL1 stacks

Every ordinary thread receives a 16 KiB kernel stack from `vm_kern`.

When the scheduler selects a thread, `arm64_switch_context()` restores that thread's SP_EL1. Exceptions from EL0 therefore enter on the kernel stack owned by the interrupted thread.

The original bootstrap thread is special: it represents the already-running boot context and continues to use the boot stack. Its current SP is captured the first time it is switched out.

### 24E — thread exit and deferred reaping

A thread cannot free the stack on which it is currently executing.

Termination therefore has two phases:

```text
thread_terminate()
    -> remove scheduling/task ownership
    -> mark TH_TERMINATE

context switch
    -> incoming stack becomes live

sched_finish_switch()
    -> reap previous terminated thread
    -> release its kernel stack
```

`processor->previous_thread` carries the outgoing thread across the machine switch. It is cleared by the incoming context.

`SYS_exit` terminates the process/task while handling the SVC. The exception return is redirected to the EL1 return trampoline created by `arm64_enter_el0()`. Once the kernel call frame is restored, the terminating thread calls `sched_exit_current()` and never runs again.

### 24F — timer preemption

The physical timer calls `sched_tick()` from the IRQ path.

Each non-idle active thread receives a four-tick quantum by default. Quantum expiration raises `processor->preemption_pending`.

NXU currently performs asynchronous context switches only when the IRQ interrupted:

- EL0, or
- the idle kernel thread.

This restriction is deliberate. General kernel preemption requires a kernel-wide preemption-disable discipline around locks and critical sections. Until that exists, an IRQ which interrupted ordinary EL1 kernel code records the pending request but does not switch stacks underneath arbitrary kernel locks.

For EL0 preemption, the exception frame stays on the outgoing thread's own kernel stack. When that thread is selected again, `arm64_switch_context()` resumes the suspended IRQ call chain, which eventually restores the exception frame and executes `eret` back to userspace.

## IRQ-safe scheduler handoff

The scheduler masks IRQs while changing processor ownership and across the actual stack switch.

This avoids a dangerous state where:

```text
processor->active_thread = new
SP_EL1 = old stack
```

could be observed by an interrupt.

A thread which resumes an older scheduler invocation restores the DAIF value saved by that invocation. A never-run thread begins at the first-run trampoline with IRQs masked; user PSTATE is then established by its saved EL0 SPSR, while kernel continuations run at the normal kernel IRQ level.

## EL0 transition ABI

`arm64_enter_el0()` preserves the kernel's x19-x30 values on SP_EL1 before `eret`.

This is required because userspace is not an AAPCS64 callee and may freely modify the registers which C considers callee-saved. The EL1 return trampoline restores those kernel values before returning to C.

Initial userspace threads use EL0t with IRQs unmasked:

```text
SPSR_EL1 = 0x0
```

The current `SYS_exit` return path uses masked EL1h while unwinding the terminating kernel thread.

## Boot-time validation

NXU performs two scheduler integration tests during boot.

First, a temporary kernel thread is created on a fresh kernel stack. The bootstrap thread yields to it, the new thread yields back, and the bootstrap thread terminates and reaps it. This validates both directions of `arm64_switch_context()` before userspace is entered.

Second, PID 1 is queued as an ordinary user thread. The bootstrap thread yields until PID 1 exits. The post-exit checks verify:

- bootstrap thread owns CPU 0 again,
- kernel_task is the current process,
- PID 1 is a zombie,
- its task is terminated,
- its initial thread is terminated,
- its kernel stack has been reaped,
- TTBR0 is disabled again.

## Next scheduler work

The current scheduler is sufficient for basic input drivers and a larger userspace bootstrap. Future scheduler work can add:

- wait queues and event channels,
- preemption-disable counters for safe kernel preemption,
- sleep deadlines,
- multiple processors and per-CPU scheduler state (MLFQ state is currently per-processor already, but only one processor exists),
- inter-processor reschedule interrupts,
- load balancing,
- MLFQ refinements: per-level time-slice accounting instead of per-level quantum length alone, and a configurable level count/boost interval instead of the fixed constants in `sched.c`.
