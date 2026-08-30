# Scheduler

NXU uses a single-processor fixed-priority scheduler. Threads are the schedulable objects, `processor_t` owns CPU-local scheduling state, and `run_queue_t` owns runnable-queue ordering.

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

`arm64_switch_context()` is implemented in `arch/arm64/context_switch.S`. It saves the outgoing context, replaces the kernel stack pointer, restores the incoming context and returns through the incoming x30.

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
- dynamic priorities,
- multiple processors and per-CPU scheduler state,
- inter-processor reschedule interrupts,
- load balancing,
- richer policy above the fixed-priority run queue.
