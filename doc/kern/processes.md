# NXU Process Management

## Overview

NXU separates UNIX process identity from execution resources.

A `struct proc` owns process identity, hierarchy, references, and termination state.

A `struct task` owns the execution container associated with that process: its userspace address space, thread collection, and task-level execution state.

The split is an ownership rule. Process identity must remain valid while execution resources are halted or torn down, and threads sharing one task must share the same userspace VM map without moving CPU state into the process object.

```text
proc
 |
 +-- identity
 |   +-- PID
 |   +-- unique ID
 |   +-- identity version
 |   `-- process name
 |
 +-- hierarchy
 |   +-- parent
 |   +-- children
 |   `-- siblings
 |
 +-- lifecycle
 |   +-- process state
 |   +-- flags
 |   `-- exit status
 |
 +-- references
 |
 `-- task
     |
     +-- address space
     +-- active / halting state
     +-- thread count
     +-- active thread count
     +-- suspend count
     `-- threads
         +-- EL0 PC/SP
         +-- kernel stack
         +-- machine state
         `-- scheduler state
```

## Process 0

PID 0 is created by `proc_bootstrap()` and represents the kernel process.

Its name is `kernel_task`.

PID 0 owns a kernel task rather than a userspace task. It therefore does not own a TTBR0 userspace address space.

The kernel process cannot exit or become a zombie.

## Process 1

The first userspace process receives PID 1 and becomes the init process.

Until a filesystem and executable loader exist, NXU constructs this process from the bootstrap EL0 image embedded in the kernel.

The special treatment concerns only how the program image is obtained. Once registered, PID 1 is otherwise represented by the ordinary process and task objects.

## Process Identity

A PID is not considered a permanent process identity.

PIDs may eventually be reused.

NXU therefore stores three identity values:

```text
pid
uniqueid
idversion
```

`pid` is the ordinary process identifier.

`uniqueid` is monotonically assigned during the current boot and identifies one specific process lifetime.

`idversion` changes whenever a new process object is created and provides another generation value for stale-reference validation.

Code that only needs traditional UNIX semantics may use a PID.

Code that must ensure it still refers to the same process should capture a `proc_ident_t` and later resolve it with `proc_find_ident()`.

## Process States

A newly allocated process starts in `PROC_STATE_EMBRYO`.

```text
EMBRYO
   |
   | executable and address space prepared
   v
RUNNABLE
   |
   | scheduler dispatch
   v
RUNNING
   |
   +--------------------+
   |                    |
   | stop               | exit
   v                    v
STOPPED               ZOMBIE
   |                    |
   | continue           | parent reap
   v                    v
RUNNABLE                DEAD
```

`EMBRYO` describes a process whose kernel object exists but which must not yet execute.

`RUNNABLE` describes a process prepared for execution.

`RUNNING` describes a process currently executing or selected as the current process.

`STOPPED` preserves the process and task while preventing execution.

`ZOMBIE` describes an exited process whose parent has not yet collected its termination information.

`DEAD` describes a reaped process which no longer participates in the process hierarchy or PID namespace.

## Exit and Zombies

`exit()` does not immediately free a process.

The task is terminated and the process is removed from the live process list. The process object is then placed on the zombie list.

The following information remains available:

```text
PID
process identity
parent
name
exit status
references
```

This permits a future `wait()`, `waitpid()` or equivalent API to collect child termination information.

Only `proc_reap()` changes a zombie into a dead process.

## Parent and Child Relationships

Every userspace process has a parent.

Children are linked through an intrusive sibling list headed by the parent's `p_children` field.

When a process exits, its children are reparented.

NXU prefers PID 1 as the new parent. If PID 1 is unavailable or is itself the exiting process, the children are reparented to PID 0.

This preserves a valid process tree even before a complete init userspace environment exists.

## References

Process existence and process references are separate concepts.

Live and zombie processes remain resident in the process manager regardless of whether external code currently holds a reference.

`proc_find()` and related lookup APIs obtain a process reference.

Every successful lookup must eventually be paired with:

```c
proc_rele(proc);
```

A process slot becomes reusable only after the process is dead and no references remain.

This prevents a stale kernel pointer from silently becoming a reference to an unrelated future process.

## File descriptors

Every process embeds one `struct filedesc` as `p_fd`.

The descriptor table belongs to process identity rather than to the task or any
individual thread. Threads in one task therefore observe the same open files
and descriptor numbers.

Each installed descriptor owns one reference to a VFS file object. Process exit
closes every installed descriptor before the proc enters zombie state. Zombie
processes retain identity and exit status, but do not retain open-file
references.

On arm64, `fork` copies the table with `filedesc_fork`: the child's descriptors
share the parent's file objects, offsets included. See
[Process control](process-control.md).

## Tasks

Every process owns one task.

The task contains the process's execution resources:

```text
task_uniqueid
active state
halting state
VM map
thread list
thread count
active thread count
suspend count
BSD proc association
```

User tasks own a `vm_address_space_t`.

Kernel tasks do not.

The task now owns an intrusive collection of threads sharing the same process address space. User PC/SP, kernel stacks and scheduler state belong to those threads rather than to the task itself.

## Current Process

`current_proc()` identifies the process associated with the current CPU execution context.

NXU currently executes on one CPU, so the implementation uses one global current-process pointer.

This is not the final SMP representation.

Once multiple processors are supported, current process and current thread state must become per-CPU data.

## Locking

The process registry uses an internal spin lock around structural mutation and reference accounting.

The current implementation assumes that process-management operations do not sleep while holding the registry lock.

As the kernel gains a blocking allocator, scheduler, signals and VFS operations, long operations must be moved outside the global process-list critical section and protected with finer-grained process/task locks.

## Process groups and sessions

Every process belongs to one process group and every group to one session
(`kern/process/pgrp.h`, `kern/process/pgrp.c`). Both are named by the pid of
the process that created them and live for as long as they have members.

A child starts in its parent's group, entered in the same critical section
that makes it visible, so it is never outside a group. A process the kernel
starts leads a new session and group of its own; PID 1 is the usual case, and
the kernel process itself leads session 0.

| Call | Effect |
| --- | --- |
| `nxu_getpgid(pid)`, `nxu_getsid(pid)` | The group or session ID of a process (0: the caller) |
| `nxu_setpgid(pid, pgid)` | Move the caller or one of its children into a group of the same session, or into a new group named after it |
| `nxu_setsid()` | Make the caller the leader of a new session and group; refused to a group leader |

`setpgid` refuses a session leader, a process in another session, a child that
has called `exec`, and a group that does not exist unless it is the one being
named after the process. A zombie stays in its group until it is reaped; a
session survives its leader. A pid is not reused while a group or session is
still named by it.

The state is protected by the process table lock. Which terminal a session
controls and which group is in the foreground is terminal state, not process
state; see [TTY subsystem](../architecture/tty-subsystem.md).

## Future Integration

The process/task boundary is intended to support later additions without changing process identity:

```text
file descriptor tables
resource limits
accounting
Mach-style IPC
task suspension
debugging
```

Those facilities should attach to either `proc` or `task` according to ownership rather than being placed arbitrarily into one large process structure.

The process object owns UNIX identity and policy.

The task owns execution resources.

