# Threads

A thread is the schedulable execution object of an NXU task. A task owns shared execution resources; each thread owns one independently schedulable CPU execution context.

The ownership hierarchy is:

```text
proc
└── task
    ├── address space
    └── threads
        ├── thread
        └── thread
```

A task owns shared execution resources such as the TTBR0 address space. A thread owns one independent flow of CPU execution.

## Thread ownership

`struct thread` contains independent linkage for:

- the global thread list,
- the owning task's thread list,
- scheduler run-queue or future wait-queue membership.

The same linkage cannot be reused for all three because a runnable thread belongs to its task and a scheduler queue simultaneously.

## Thread state

NXU uses composable state bits:

```text
TH_WAIT
TH_SUSP
TH_RUN
TH_UNINT
TH_TERMINATE
TH_IDLE
TH_WAKING
```

Kernel-only flags currently include:

```text
TH_FLAG_KERNEL
TH_FLAG_REAPED
TH_FLAG_BOOTSTRAP
```

## Machine state

Each thread embeds `machine_thread_t`.

Its kernel context contains the callee-saved state required by `arm64_switch_context()`:

```text
x19-x28
fp
lr
sp
```

Its user state currently stores the initial:

```text
PC
SP_EL0
SPSR_EL1
```

Once a user thread is executing inside an exception path, the live general-purpose user register state is represented by the exception frame on that thread's kernel stack.

## Kernel stacks

Normal threads receive a 16 KiB kernel stack allocated from `vm_kern`.

The stack is used for:

- scheduler continuation frames,
- system calls,
- exceptions from EL0,
- IRQ frames while that thread is current,
- blocked kernel call chains.

The bootstrap thread is the only exception. It represents execution which existed before the thread subsystem and uses the original kernel boot stack.

## First dispatch

A never-run thread has a synthetic kernel context prepared before it becomes runnable.

Its saved SP points at the top of its kernel stack and its saved LR points at the scheduler first-run trampoline.

For a user thread the trampoline enters EL0 using the thread's own PC, SP_EL0 and SPSR_EL1. For a kernel thread it calls the stored `thread_continue_t` function.

## Termination

`thread_terminate()` makes a thread ineligible for execution and detaches it from its task.

`thread_reap()` is separate. A current thread may not reclaim its own kernel stack. The scheduler performs deferred reaping only after another thread's stack has become active.

This separation is required for safe `SYS_exit` and remains useful once blocking, cancellation and multi-processor scheduling are added.
