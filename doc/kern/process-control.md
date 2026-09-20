# Process Control: Fault Containment, Signals, `fork` and `exec`

This page covers how a user process is created, replaced, signalled and ended
on arm64. The i386 port shares the code but not the machine-dependent half:
there `fork`, `exec`, `kill`, `sigaction`, `sigprocmask` and `sigreturn` return
`NXU_SYS_E_NOT_SUPPORTED`, and a user fault still ends the process with the
CPU trap vector in its exit status (`mach/i386/syscall_trap.c`).

## A user fault kills the process, not the kernel

A synchronous exception from EL0 that is not a system call reaches
`exception_handle_user_fault()` in `mach/arm64/exception.c`. In order:

1. **Try to resolve it.** A data or instruction abort of translation, access
   flag or permission kind is offered to the page-fault resolver
   ([Demand paging and copy-on-write](../vm/demand-paging.md)). If that maps or
   copies the page, the exception simply returns and the instruction retries.
2. **Turn it into a signal.**

   | Exception | Signal |
   | --- | --- |
   | translation, permission or access-flag abort | `SIGSEGV` |
   | alignment fault, external abort, PC or SP alignment | `SIGBUS` |
   | undefined instruction, anything unrecognised | `SIGILL` |
   | `brk` | `SIGTRAP` |
   | floating-point trap | `SIGFPE` |

3. **Deliver or kill.** If the program installed a handler for that signal and
   does not have it blocked, the handler runs in the faulting thread. Otherwise
   the whole process is terminated and its exit status is
   `NXU_EXIT_KILLED_SIGNAL(signal)` — bit 31 set, signal number in the low byte —
   which `waitpid` reports unchanged. The kernel logs one line naming the pid,
   program, signal and the `pc`/`far`/`esr` values.

Only a fault with no user process to blame — one taken by the kernel task
itself, or an SError/FIQ — still reaches the panic path. A fault taken at EL1
on a *user* address (a syscall or self-test dereferencing user memory
directly) is also offered to the resolver first, so lazily mapped memory works
there too.

`kern/tests/fault_process_test.c` proves this with eight cases, including a
fault on a secondary thread and a clean exit, all in one boot.

## Signals

The model is deliberately small; `kern/process/signal.h` is the reference.

- **State.** Dispositions (`proc::p_sigact`) and the pending set
  (`proc::p_sigpending`) belong to the process; the blocked mask
  (`thread::sig_blocked`) belongs to the thread. A signal sent to a process is
  taken by whichever of its threads next returns to user mode without blocking
  it.
- **Dispositions.** Default, ignore, or a user handler with a *restorer* — a
  tiny routine ending in `sigreturn`, supplied by `nxu_signal()` in libnxu. The
  default action terminates the process, except `SIGCHLD`, which is ignored.
  `SIGKILL` cannot be caught, blocked or ignored.
- **Who may signal whom.** A process may signal itself and its descendants
  (`proc_is_inferior`). There are no credentials, so this is what keeps an
  ordinary process from killing PID 1 or a sibling service. The kernel process
  is never a target. `kill(pid, 0)` only checks existence and permission.
- **When a signal takes effect.** A terminating signal aimed at another process
  ends it immediately: nothing else can be running on the single CPU, so the
  target is either runnable or asleep on a wait queue, and `sched_thread_terminate`
  unlinks a sleeper from its queue first (see *Sleeping* below). A caught signal, or one aimed at the sender itself, is delivered on the
  next return to EL0 — after a system call or an interrupt — by
  `exception_deliver_pending_signals()`.
- **Delivery.** `machine_user_signal_push()` saves x0–x30, sp, pc and the flags
  in a `arm64_sigframe_t` on the user stack (below the interrupted `sp`, 16-byte
  aligned), then points the thread at the handler with `x0 = signal` and
  `x30 = restorer`. The thread's blocked mask gains the signal (unless
  `NXU_SA_NODEFER`) and the action's extra mask. When the handler returns, the
  restorer issues `sigreturn`, which reads the frame back from `sp`. The frame is
  in user-writable memory, so `machine_user_signal_pop()` checks its magic and
  that `pc` and `sp` stay in user space, and forces the restored PSTATE to plain
  EL0. A bad frame kills the process with `SIGSEGV`.
- **`SIGCHLD`.** When a child exits, a parent that installed a handler has the
  signal made pending. A parent that did not pays nothing.
- **Not implemented:** `SIGSTOP`/`SIGCONT` (refused with `NOT_SUPPORTED`),
  timers and `SIGALRM`, `sigsuspend`/`pause` (the wait queue it needs exists, the
  syscall does not), signal queueing (a signal already pending is not counted twice), per-thread
  directed signals, `siginfo`, alternate signal stacks, and floating-point state
  in the frame (userland is built `-mgeneral-regs-only`).

## Sleeping: wait queues

`kern/sched_prism/waitq.{c,h}` is the one way a thread sleeps until something
happens. The pattern is always *check, join, block, check again*: a wakeup means
"look again", never "it is true".

```c
for (;;) {
    if (condition()) break;
    if (!waitq_block(&queue, true)) return interrupted;
}
```

| Wait | Queue | Woken by |
| --- | --- | --- |
| `nxu_wait(pid, &status)` | `proc->p_waitq` of the waiting parent | a child exiting (`proc.c`, wakes the parent's whole queue) |
| `nxu_ipc_receive_wait(port, ...)` | `port->ip_waiters` | a message being enqueued on the port (wakes one receiver) |
| socket `connect`, `accept`, read, write | `socket->waiters` | the peer's matching action or a close |

- **Interruptible waits.** `waitq_block(queue, true)` marks the sleeper as
  interruptible. Sending it a signal calls `waitq_interrupt`, which unlinks it
  and makes it runnable; `waitq_block` returns false and the syscall returns
  `NXU_SYS_E_INTERRUPTED` (`EINTR`), after the signal's handler has run. `nxu_wait`
  also checks for an already pending signal before sleeping, so a signal that
  arrived between the check and the block is not slept through.
- **Death while asleep.** `sched_thread_terminate` calls `waitq_remove`, so a
  killed sleeper leaves its queue before it is torn down and no queue ever
  points at a dead thread. `proctest` (checks 68-85) interrupts a sleeping
  `nxu_wait` with a signal, kills a process asleep in `nxu_ipc_receive_wait` with
  `SIGKILL` (it is reaped with a `SIGKILL` status and later sleeps still work),
  and wakes a blocked receiver with a message from a child.
- **Socket waits are not interruptible.** The socket loops re-check their
  condition on every wakeup but have no way to report "interrupted" to their
  callers, so a signal does not break a blocked `connect`/`accept`; a terminating
  signal still kills the process.
- **The non-blocking calls still exist.** `nxu_waitpid` and `nxu_ipc_receive`
  return "try again" as before; the `_wait` forms are the sleeping versions.
- **Single CPU only.** The queue has no lock: checking the condition and joining
  the queue are atomic only because the kernel is not preemptible and wakeups come
  from thread context. SMP will need a real lock in `waitq.c`.

## `fork`

`proc_fork()` makes a child that is a copy of the caller.

| Resource | Child gets |
| --- | --- |
| Address space | Copy-on-write copy of every private writable page; read-only and executable pages shared as they are; the shared-memory window stays genuinely shared. |
| Anonymous regions | A copy of the region list and placement cursor. Pages nobody touched stay untouched in both. |
| Open files | The same file objects, so parent and child share offsets. |
| Port names | The same names, each holding its own reference. Ports are not split into send and receive rights, so both processes hold the full capability, like an inherited pipe. |
| Signals | Copied dispositions, no pending signals, the calling thread's blocked mask. |
| Threads | One: a copy of the calling thread, resuming exactly where `fork()` returns with `x0 = 0`. |

The child's first thread starts with a complete register file
(`machine_thread_t::user.full`, entered through `arm64_enter_el0_regs`) taken
from the exception frame the parent trapped with (`machine_thread_t::user_frame`,
set on every EL0 trap). The parent's `fork()` returns the child's PID.

Failure is clean: any step that cannot complete undoes the ones before it, and
a page already marked copy-on-write simply copies or reclaims itself on its next
write.

## `exec`

`loader_exec()` replaces the running program in place.

1. Read and validate the ELF image, as `loader_spawn` does.
2. Build the whole new image — segments, a reserved stack region, `argv` laid
   out at the top of the stack — in a **separate address space**. The old
   program is untouched until this succeeds, so every failure just returns an
   error to the caller.
3. Rewrite the calling thread's user registers (`machine_user_exec_state`): the
   new entry and stack, everything else zero, `x0 = argc`, `x1 = argv`.
4. Commit: deactivate the old address space, move the new one into the task,
   activate it, then release the old image's pages and tables.
5. Caught signals revert to the default; ignored ones stay ignored. Open
   files and port names carry over. The process is renamed to the new
   program's file name.

`exec` refuses a process with more than one thread (`NXU_SYS_E_BUSY`), accepts
at most 16 arguments and 1 KiB of argument text, and needs an executable
whose `PT_LOAD` segments fit below the stack.

## Address-space reclamation

`task_terminate()` now returns every page and page-table a user task owned:
regions first, then `vm_address_space_release_pages()` (one reference per
mapping, so a page shared by `fork` or shared memory survives until its last
owner goes), then `vm_address_space_destroy()`. A task that is the one
currently running is deactivated first. Before this, arm64 leaked address
spaces at exit.

## Tests

| Test | Proves |
| --- | --- |
| `make test TEST=fault-process` | Eight faulting and clean programs; the kernel survives all of them. |
| `make test TEST=process-control` | `proctest` (85 checks): fork, copy-on-write both ways, shared memory across fork, demand paging, lazy stack, exec with argv, handlers, masks, ignore, kill, `SIGCHLD`, a `SIGSEGV` handler, the descendants-only rule, blocking `wait` and `ipc_receive_wait`, `EINTR`, and killing a process that is asleep in the kernel. Afterwards the kernel checks that the physical pages in use returned to within 24 of where they started, across 17 forks and an exec. |
