# Process Control: Fault Containment, Signals, `fork` and `exec`

This page covers how a user process is created, replaced, signalled and ended
on arm64. The i386 port shares the code but not the machine-dependent half:
there `fork`, `exec`, `kill`, `sigaction`, `sigprocmask` and `sigreturn` return
`NXU_SYS_E_NOT_SUPPORTED`, and a user fault still ends the process with the
CPU trap vector in its exit status (`kern/i386/syscall_trap.c`).

## A user fault kills the process, not the kernel

A synchronous exception from EL0 that is not a system call reaches
`exception_handle_user_fault()` in `kern/arm64/exception.c`. In order:

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
  default action terminates the process, except: `SIGCHLD` and `SIGWINCH` are
  ignored; `SIGSTOP`, `SIGTSTP`, `SIGTTIN` and `SIGTTOU` stop the process;
  `SIGCONT` continues it. `SIGKILL` and `SIGSTOP` cannot be caught, blocked or
  ignored.
- **Who may signal whom.** Credentials decide: root may signal anyone; anyone
  else may signal a process whose real or saved user ID equals the sender's
  real or effective one; `SIGCONT` may also go to any process in the sender's
  session. The kernel process is never a target, and PID 1 only takes signals
  it has installed a handler for. `kill(pid, 0)` only checks existence and
  permission. `killpg(pgid, signal)` sends to every member of a process group.
- **A process acts on its own signals.** Whatever a signal does to a process
  (run a handler, stop it, end it) is done by one of its own threads in
  `signal_take`, as it is about to return to user mode. Sending is
  `signal_post`: mark the signal pending, wake interruptible sleepers, kick
  running threads. `signal_post` never ends or stops anything, never sleeps,
  and is what kernel code (the terminal) uses. `kill(2)` is the one exception,
  below.
- **Stopping and continuing.** A stopped process has every thread parked at its
  return to user mode, on a per-process wait queue; a thread asleep in the
  kernel parks when its system call finishes. `SIGCONT` continues a stopped
  process the moment it is sent, whatever its disposition, and discards pending
  stop signals; a stop signal discards a pending `SIGCONT`; `SIGKILL` ends a
  stopped process without continuing it. A counter of `SIGCONT`s closes the
  race between taking a stop signal and acting on it. The parent is sent
  `SIGCHLD` and can collect both changes with
  `nxu_wait_options(pid, &status, NXU_WAIT_UNTRACED | NXU_WAIT_CONTINUED_OPTION)`;
  `NXU_WAIT_NOHANG` makes that call not sleep.
- **Orphaned process groups.** A group is orphaned when no member has a parent
  in another group of the same session: nobody is left who could resume it. A
  stop asked for from the terminal (`SIGTSTP`, `SIGTTIN`, `SIGTTOU`) is not
  acted on in such a group, and when an exit orphans a group that has stopped
  members, they are sent `SIGHUP` and then `SIGCONT`.
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
- **Not implemented:** timers and `SIGALRM`, `sigsuspend`/`pause` (the wait queue it needs exists, the
  syscall does not), signal queueing (a signal already pending is not counted twice), per-thread
  directed signals, `siginfo`, alternate signal stacks, and floating-point state
  in the frame (userland is built `-mgeneral-regs-only`).

### Per architecture

| | arm64 | i386 |
| --- | --- | --- |
| Handlers, `sigreturn`, masks | yes | yes (`kern/i386/user.c`) |
| Stop, continue, `killpg`, wait options | yes | yes |
| A fault in user code becomes a signal a handler can catch | yes | no: the process is ended directly |
| `fork`, `exec` | yes | no |

A port declares the first two rows with `MACHINE_USER_SIGNALS` and the last
with `MACHINE_USER_CONTEXT` (`kern/machine/user.h`).

### Restarting interrupted system calls

A system call that sleeps interruptibly (a terminal read or write, an open
waiting out a terminal's driver, `TIOCDRAIN`, a wait for a child) ends with
"interrupted" when a signal arrives. What the program then sees is decided on
the way back to user mode, after the signal has been acted on:

| The call | The signal | Result |
| --- | --- | --- |
| had moved data (a partial read or write) | any | the short count; never made again |
| had done nothing, restartable | runs a handler with `NXU_SA_RESTART` | made again after the handler |
| had done nothing, restartable | runs a handler without it | `-NXU_SYS_E_INTERRUPTED` after the handler |
| had done nothing, restartable | runs no handler (stop and continue) | made again |
| not restartable | any | `-NXU_SYS_E_INTERRUPTED` |

Restartable: `read`, `write`, `open`, `ioctl`, `wait`, `wait_options`,
`socket_connect`, `socket_accept` (`syscall_is_restartable` in
`kern/syscall/syscall.c`). Not restartable, on purpose: `sleep_us`, which a
caught signal now ends early as `nanosleep` is ended, the event wait and the
message waits, which are how a program waits for "anything", a signal included.

How it works, so that only the last step is per architecture:

1. `syscall_dispatch` (machine-independent) turns an "interrupted" result of a
   restartable call into `SYSCALL_ACTION_RESTART` and marks the thread
   (`thread->syscall_restart`). The trap path stores no result, so the user
   registers still describe the call exactly as it was made.
2. The return-to-user path (`exception_deliver_pending_signals` on arm64,
   `i386_deliver_pending_signals` on i386) calls `signal_take`, then
   `syscall_restart_resolve` with the handler it is about to run, if any,
   before it builds the handler frame.
3. It applies the answer: `AGAIN` moves the user program counter back over the
   trap instruction (`svc`, 4 bytes; `int $0x80`, 2 bytes), `INTERRUPTED`
   stores the error as the result. Because this happens before the handler
   frame is built, the context the frame saves is the one `sigreturn` comes
   back to, and the call is made again from there.

A call reports "interrupted" only from a sleep it took before changing
anything, which is what makes making it again invisible; a transfer that had
moved bytes returns them (`syscall_io_partial`). `sigaction` refuses flags it
does not know (`NXU_SA_FLAGS_ALL`). `nxu_signal` installs handlers without
`NXU_SA_RESTART`, as it always has.

Tests: `restarttest` (`frameworks/BootDaemons.framework/restarttest.c`), run on
a loopback terminal by `kern/tests/termios_process_test.c`: a terminal read
interrupted by a handler with and without `SA_RESTART`, by a stop and a
continue, a partial write (short count with and without the flag, nothing
written twice), a write that had moved nothing restarted under `SA_RESTART`, a
sleep that is not restarted, and an unknown flag refused. i386 boot case
`restart`; arm64 `tty` row.

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

## Capabilities

NXU has no credentials (no UID/GID, no file owners or modes), so the system calls
that change the machine are gated by a per-process capability mask,
`proc->p_caps`. It is default-deny.

| Capability | Allows |
| --- | --- |
| `NXU_CAP_FS_WRITE` | `open` with write, create, truncate or append; `unlink`; `mkdir` |
| `NXU_CAP_DISPLAY` | `display_claim` |
| `NXU_CAP_RESET` | `system_reset` (syncs filesystems before reboot; available to authorized mainOS and recovery processes) |
| `NXU_CAP_AUDIO` | opening `/dev/audio0` (writing a device node does not need `NXU_CAP_FS_WRITE`; create, truncate and append still do) |

A refused call returns `-NXU_SYS_E_DENIED`. Reading is never gated. `open` is
checked on the same 32 flag bits `vfs_open` receives, so a high bit cannot get
past the check.

Where a process's set comes from:

- **PID 1** (bootd, or triageOS in recovery) and the kernel process hold all of
  them. Every other process starts with none.
- **`nxu_spawn_caps(path, name, caps)`** gives the child exactly `caps`. That must
  be a subset of the caller's own: asking for more fails with `DENIED` rather than
  being silently narrowed, so a misconfigured service fails loudly, and a bit
  that is not a capability is `INVALID_ARGUMENT`. Plain `nxu_spawn` passes none.
- **`fork` and `exec`** keep the caller's set.
- **bootd** passes what a service's plist asks for, `AllowFilesystemWrite` and
  `AllowDisplay` (see [service plists](../service-plists.md)). `logd` and `patchd`
  have the first, `windowserver` the second; nothing else can write the
  filesystem or claim the display.
- **Kernel-started test processes** hold none until the test grants some with
  `proc_set_caps`.
- **`nxu_get_caps()`** returns the caller's set.

What this is not:

- It is not per-file authorization. A process with `FS_WRITE` may write any file.
- A process cannot shed capabilities it holds; it can only give a child less at
  spawn.
- Signals follow credentials (above), not capabilities.
- PID 1 is identified by its PID number, as `ipc_register_bootstrap` already does.
  Nothing reuses it while bootd is alive.

`proctest` checks 86-93 (an ordinary process is refused each call, and can still
read) and `privtest` (a granted parent uses what it holds and is refused the
rest; a child gets exactly the subset passed; `fork` and `exec` keep the set)
cover it.

## Spawn with attributes

`nxu_spawn_attr(path, &attr)` starts a program with everything it is to begin
with already in place: its arguments, the descriptors it inherits (each named
as "this number in the child is that open file of mine"), its process group
(the caller's, a new one, an existing one of the caller's session) or a new
session, and its signal mask. The kernel applies all of it while the process
is an embryo that cannot run, with group placement decided in the critical
section that makes it visible, and starts it only afterwards; a request that
cannot be honoured leaves no process behind. This is what a parent needs on a
port without `fork`, where nothing can be adjusted between creating a child
and running it, and it closes the usual races on a port that has `fork` too.

Every process starts with its stack pointer at `argc`, with `argv` in the
words above it, whether spawned or reached by `exec`; the entry stubs in
`frameworks/crt0.S` and `frameworks/crt0_i386.S` pass them to `main`.

`nxu_spawn_io(path, argv, &stream)` builds on it: the child's standard input,
output and error are one end of a socket pair (`nxu_socketpair`) and the caller
keeps the other.

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
| `make test TEST=process-control` | `proctest` (93 checks): fork, copy-on-write both ways, shared memory across fork, demand paging, lazy stack, exec with argv, handlers, masks, ignore, kill, `SIGCHLD`, a `SIGSEGV` handler, the descendants-only rule, blocking `wait` and `ipc_receive_wait`, `EINTR`, killing a process that is asleep in the kernel, and an ordinary process being refused the calls that need a capability. `privtest` then covers a process that was granted some. Afterwards the kernel checks that the physical pages in use returned to within 24 of where they started, across 17 forks and an exec. |
