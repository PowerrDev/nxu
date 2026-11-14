# TTY, PTY and termios: audit and plan

Status: **Phase 0 (audit) complete and approved. Phase 1 (infrastructure, M1 to M7) complete. Phases 2 to 5 (TTY core, line discipline, termios, job control) built and committed. Phase 6 (PTY, and Terminal.app, nzh, su, sudo and passwd moved onto it) built, not yet committed. Next: Phase 7, hardware backends, or Phase 8, hardening.**

This document is the prerequisite audit, dependency report and roadmap for a
kernel terminal subsystem (TTY core, line discipline, termios, PTYs, job
control). Phase and milestone numbers live here only, never in source files.

Every finding below was read from the code at `main` (502119a), not inferred
from names. File and line references are to that revision.

## 1. What exists today

There is no kernel terminal of any kind. What looks like one is this:

- `Terminal.app` (`frameworks/Terminal.framework/terminal.c`) does line editing,
  history and echo itself and sends whole lines to the shell.
- The shell's stdio is a **local stream socket** (`kern/ipc/socket.c`). On i386
  `nxu_spawn_io` (`frameworks/lib/spawn_io_i386.c`) listens on
  `com.nxu.spawn.<pid>`, spawns the child, and the child's crt0 connects back,
  reads its **argv** from the socket, then `dup2`s it onto 0/1/2.
- Ctrl-C is `nxu_kill(shell, SIGINT)` on arm64 and an in-band byte `0x03` on
  i386 (`terminal.c:531`), which `nzh` polls for (`nzh.c:158`, `:728`).
- The kernel console (`kern/console/console.c`) is an output-only log sink.
  The UART (`platform/*/uart.c`) is polled, transmit-only: there is no receive
  path and no UART interrupt on any architecture.

## 2. Audit findings

### 2.1 VFS, files, descriptors

| Item | State | Evidence |
| --- | --- | --- |
| vnode abstraction, refcounts, ops table | READY | `vfs/vnode.h` |
| `open`/`close`/`ioctl` vnode ops for devices | READY | `vfs/vnode.h:114`, `vfs/vfs_file.c:37` |
| File objects, shared on fork, `dup2` | READY | `vfs/file.c` |
| ioctl encoding and copy-in/out | READY | `NXU_IOC*` in `syscall_defs.h`, `syscall_ioctl` |
| ioctl on descriptors 0-2 | INCOMPLETE | `syscall_ioctl` rejects `descriptor < VFS_FD_FIRST_FILE` (`syscall.c:673`), so `tcgetattr(0)` cannot work |
| Per-call I/O flags | MISSING | `vnode_read`/`vnode_write` take no flags. `O_NONBLOCK` reaches a device only at `open` (`virtio_sound_dev.c:103`), so it is per device, not per open file |
| Per-open device state / cloning open | MISSING | `struct file` has no private pointer and `open` cannot substitute a vnode; `/dev/ptmx` cannot hand each opener its own master |
| Short-read semantics | INCOMPLETE | `syscall_read` loops per 4096-byte chunk until a short count (`syscall.c:588`); a terminal read that returns exactly one full chunk blocks again for more |
| stdio descriptors | INCOMPLETE | 0-2 are special: never allocated by `filedesc_install`, and `syscall_write` falls back to `kputc` when 1/2 are empty (`syscall.c:540`) |
| `fcntl`, `dup`, `poll`/`select`, `O_NOCTTY`, `isatty` | MISSING | no such syscalls; `read_poll` is socket-only (`syscall.c:2572`) |
| Error codes | INCOMPLETE | no "not a terminal" / "no such device" status in `vfs_status_t` |
| Concurrency | CAVEAT | VFS has no locking; every syscall that reaches it is bound to the boot CPU (`syscall_is_cpu_agnostic`, `syscall.c:2444`). Blocking inside a bound syscall is established practice (sockets, audio) |

### 2.2 Character devices (devfs)

`vfs/devfs.c` is a flat table of at most **8** devices (`DEVFS_DEVICE_MAX`),
with one registered user (`audio0`).

| Item | State |
| --- | --- |
| Name → ops + context registration | READY |
| Unregistration, node lifetime | MISSING: entries are never removed; vnodes are embedded in the table |
| Dynamic nodes (one per PTY) | MISSING |
| Subdirectories (`/dev/pts/`) | MISSING: `devfs_lookup` only accepts the root |
| Ownership, mode, `getattr` | MISSING: no `getattr`, so `vnode_access` does not enforce anything on devices and `grantpt` has nothing to set |
| Open flags / caller identity in read, write | MISSING: ops receive only `context` |

### 2.3 Processes

| Item | State | Evidence |
| --- | --- | --- |
| proc table, PIDs, idents, refcounts, parent/child tree | READY | `kern/process/proc.c` |
| Exit, zombie, reap, blocking `wait` | READY | `proc_exit`, `syscall_wait` |
| Process groups | MISSING | no pgid anywhere in the tree |
| Sessions, session leaders, controlling terminal | MISSING | |
| Stop / continue | MISSING | `proc_stop`/`proc_continue` only flip `p_stat`; they have no callers and no scheduler effect |
| `wait` reporting stopped children | MISSING | |
| Orphaned process group handling | MISSING | |
| fork / exec | arm64 only | `MACHINE_USER_CONTEXT` is 0 on i386 (`kern/i386/user.h:21`) |
| Spawn: argv, descriptor inheritance | MISSING | `loader_spawn_internal` gives the child an empty descriptor table and no argv; the i386 argv path is the spawn_io socket |
| x86_64 | N/A | `makedefs/x86_64.mk` builds a bare kernel: no VFS, no processes, no scheduler |

### 2.4 Signals

| Item | State | Evidence |
| --- | --- | --- |
| Dispositions, pending set, per-thread mask, `sigaction`/`sigprocmask`/`sigreturn` | READY (arm64) | `kern/process/signal.c` |
| Interrupting interruptible sleeps | READY | `signal_interrupt_sleepers` → `waitq_interrupt` |
| Handler delivery on return to user | arm64 only | `signal_dequeue` is called only from `kern/arm64/exception.c`. On i386 the `kill`, `sigaction`, `sigprocmask` syscalls are compiled out entirely, and Activity Monitor's Kill is a no-op (`ui_session_manager.c:464`) |
| `SIGTSTP`, `SIGTTIN`, `SIGTTOU`, `SIGWINCH` | MISSING | not defined |
| `SIGSTOP` / `SIGCONT` | MISSING | refused (`signal.c:169`) |
| Default actions | INCOMPLETE | two-valued: terminate, or ignore for `SIGCHLD` |
| Process-group delivery | MISSING | |
| Permission model | INCOMPLETE | sender must be an ancestor (`proc_is_inferior`); written before credentials existed |
| Kernel-originated signals | INCOMPLETE | done by passing `proc_kernel()` as sender |
| Execution context | CAVEAT | a default-terminate signal runs `proc_exit(target)` synchronously in the sender (`signal.c:201`); that binds to the boot CPU and sleeps, so it cannot run in interrupt context or under a spinlock |
| `SIGCHLD` | INCOMPLETE | set pending on exit only, and without waking sleepers |

Terminating another process by default action needs no machine support, so
most of the i386 gap is a gate that is wider than it needs to be. Only
*handler* delivery needs `machine_user_signal_push/pop` for i386.

### 2.5 Scheduler and synchronization

| Item | State | Evidence |
| --- | --- | --- |
| Wait queues, interruptible sleep | READY | `kern/sched_prism/waitq.c` |
| Lost-wakeup-safe sleep on SMP | READY | `waitq_block_seq`, `waitq_block_unlock` |
| Terminated threads leave their queue | READY | `sched_thread_terminate` → `waitq_remove` |
| Spinlocks, irqsave forms | READY | `kern/lock.h` |
| Sleeping mutex, condition variable | MISSING | none in the kernel; every lock is a spinlock |
| Timed wait on a specific queue | MISSING | only the global tick queue (`sched_sleep_us`) and the global kick queue exist; `VTIME` and `tcdrain` need a deadline on a per-object wait |
| Deferred work from interrupt context | MISSING | no work queue or thread call |

### 2.6 Memory

READY. `kmalloc`/`kcalloc`/`kfree` return null on failure. Object lifetime is
hand-rolled refcount-under-spinlock per object (`socket.c`, `ipc_port.c`); the
TTY objects follow the same pattern. No special allocator is needed.

### 2.7 Device and console infrastructure

| Item | State |
| --- | --- |
| UART transmit | READY (polled) |
| UART receive, UART interrupts | MISSING on all architectures |
| Kernel console input | MISSING; the console is a log sink |
| Keyboard | events go to the UI session (`drivers/input`), not to a text console |

Kernel logging does not depend on any terminal and will not: `kputc` stays as
it is.

### 2.8 Test infrastructure

READY. Four layers (`doc/testing.md`): boot self-tests, kernel test configs
(`make test TEST=<id>`), i386 suites (`tools/test_i386*.sh`), and host tests of
pure code under ASan+UBSan. The line discipline can be written as pure code and
host-tested the way `tools/btrfs` is.

## 3. Dependency graph

```
 A. vnode I/O flags ─────────────┐
 B. cloning open / per-open ─────┤
 C. devfs: dynamic nodes, attrs ─┼──► TTY core ──► line discipline ──► termios
 D. stdio 0-2 as ordinary fds ───┤        │
 E. waitq timed wait ────────────┘        │
                                          ▼
 F. pgrp + session (proc) ──┐           PTY ──► Terminal.app / nzh on a PTY
 G. signals: stop/cont,     ├──► job control ──┘
    pgrp send, new numbers ─┤
 H. i386 handler delivery ──┤   (H gates job control on i386 only)
 I. spawn attributes ───────┘   (argv, fd inheritance, pgrp/session/ctty)

 J. UART receive + deferred input ──► serial / console backends
```

F must precede G (group delivery needs groups). I depends on D and F. Job
control depends on F, G, I, and on i386 also H. The PTY needs A, B, C. J is
independent of everything but the TTY core.

## 4. Architectural risks

1. **i386 is spawn-only.** Unix sets up a job between `fork` and `exec`
   (`setpgid`, `tcsetpgrp`, reset dispositions, redirect descriptors). With no
   fork on i386 that must all be expressed as **spawn attributes**, in the
   manner of `posix_spawn`. This is the largest single prerequisite and it
   replaces the spawn_io socket relay, including how argv is delivered.
2. **Signals on i386.** Job control with a shell that catches `SIGINT` and
   `SIGCHLD` needs real handler delivery on i386 (item H). Without it i386 gets
   terminal-generated *termination* only.
3. **Signal generation sleeps.** `signal_send` may run `proc_exit`. The line
   discipline must therefore post signals from thread context with no TTY
   spinlock held; input from an interrupt (UART) must be handed to a thread
   first.
4. **No sleeping locks.** TTY state is protected by spinlocks, and anything that
   can block (a driver's output, a signal) is called after they are dropped.
   This is workable (sockets do it) but constrains the driver interface.
5. **No `poll`.** Terminal.app multiplexes tabs with `read_poll` plus a frame
   sleep. The PTY master will support `O_NONBLOCK` and wake the existing kick
   sequence (`NXU_SYS_EVENT_WAIT`), which matches how UI clients already wait.
   A general `poll` is out of scope and recorded as a limitation.
6. **vnode op signature change** (item A) touches ext4, btrfs, ramfs and devfs
   mechanically.
7. **The flag day.** Moving Terminal.app and nzh from sockets to a PTY changes
   crt0, spawn, the shell and the emulator together. It is staged last, behind
   working kernel tests, with the socket path kept until the PTY path passes.

## 5. Decisions (approved)

- **Device naming: Darwin style.** `/dev/ptmx`, slaves `/dev/ttysNNN`,
  `/dev/tty`, `/dev/console`. This keeps devfs flat (no directory support
  needed) and matches NXU's lineage. `ptsname()` hides the name, so a
  `/dev/pts/` layout stays possible later.
- **termios ABI: fixed-width.** `tcflag_t` and `speed_t` are `uint32_t`,
  `cc_t` is `uint8_t`, `NCCS` 20; `struct termios` is 44 bytes with no padding
  and is identical on arm64 and i386. Requests are `NXU_IOC` group `'t'`.
- **Signal numbers** continue the existing Linux numbering: `SIGTSTP` 20,
  `SIGTTIN` 21, `SIGTTOU` 22, `SIGWINCH` 28.
- **x86_64** has no VFS or processes; the TTY is not linked there. The
  requirement is only that its build stays green.
- **Source layout**: `kern/tty/` (`tty.c`, `tty_ioctl.c`, `tty_ldisc.c`,
  `n_tty.c`, `tty_jobctl.c`, `pty.c`, private `tty_internal.h`), public ABI in
  `kern/syscall/termios_defs.h` re-exported to userland as
  `frameworks/include/termios.h`. Fewer, larger files than the suggested tree,
  matching `kern/ipc/`.

## 6. Design rules that cut across milestones

These three were set as requirements when the plan was approved. Each is a
structural property, not a convention to remember.

### 6.1 Readiness notification

General `poll`/`select` is out of scope, but nothing built here may have to
change shape when it arrives. So readiness is a generic mechanism from the
start, in two halves:

- **A level query.** A vnode operation `poll(vnode, &ready)` reports, without
  sleeping, which of *readable*, *writable*, *hung up* and *error* hold right
  now. Sockets answer the same question through the file layer
  (`file_poll`).
- **An edge notifier.** Every object that can become ready embeds one
  `ioready_t` and calls `ioready_post(&object->ready, events)` when it goes
  from not ready to ready, after dropping its own lock. Interested parties
  attach to the notifier; producers never know who is listening.

Today the only listener is the kick sequence (`NXU_SYS_EVENT_WAIT`), so a
Terminal tab sleeping for UI events also wakes when its PTY master becomes
readable, and then asks with a nonblocking read. A future `poll` attaches one
waiter record per descriptor to the same notifiers and uses the same level
query: no driver, PTY or TTY code changes. This is the role `selinfo` and
`selwakeup` play in XNU.

The notifier landed with M3 (`kern/ioready.h`); the TTY and PTY use it from
their first version.

### 6.2 Process creation is atomic with respect to execution

A spawned process must already be in its session and process group, hold its
inherited descriptors, have its controlling terminal and its signal
dispositions **before its first thread can run on any CPU**. The parent must
never have to fix a child up after the fact (the classic race in which a
child runs, or is signalled, before `setpgid` has been applied to it).

The spawn-attributes call therefore does all of it while the child is still
an embryo: the process is built, configured and validated first; group and
session membership are entered in the same process-table critical section
that makes the process visible; only then is its thread made runnable. Any
failure before that point unwinds completely, so a half-configured process is
never visible, signalable or runnable. Changing the foreground group to the
new job is one of the attributes for the same reason.

### 6.3 Terminal code never tears a process down

`signal_send` can run `proc_exit` on its target, which binds to the boot CPU,
sleeps, and closes descriptors (possibly this very terminal). Calling that
with a TTY lock held would deadlock or recurse.

Two layers prevent it:

1. **A posting primitive that cannot tear down** (M5). `signal_post` and its
   process-group form only mark the signal pending and wake interruptible
   sleepers. They take leaf spinlocks, never sleep, and never call
   `proc_exit`. A fatal default action is carried out by the *target*, in its
   own context, when it next crosses the kernel boundary (return to user
   mode, or an interruptible sleep returning "interrupted"). The synchronous
   kill-now path stays, but only behind `kill(2)` for a target that may be in
   an uninterruptible sleep, where the caller is in plain syscall context.
2. **The TTY never signals under its locks anyway.** Input processing records
   what must be signalled (which group, which signal), releases every TTY
   lock, and only then posts. The same applies to hangup and to
   `SIGTTIN`/`SIGTTOU`.

Process exit reaching back into the terminal (a session leader exiting,
closing the last descriptor) happens from `proc_exit` with no TTY lock held
by construction, since no terminal path can be on the stack beneath it.

## 7. Roadmap

Each milestone lands with its tests and leaves all three builds and
`make check` green.

**Phase 1: infrastructure**

| # | Milestone | Covers | Test |
| --- | --- | --- | --- |
| M1 ✅ | vnode read/write I/O flags; stream reads asked once; ioctl on 0-2; `read_poll` on any stream | A, D (part) | existing suites (below) |
| M2 ✅ | devfs: dynamic register/unregister, 64-slot table, uid/gid/mode + `getattr`/`setattr`, cloning open | B, C | `devfs_self_test` at boot |
| M3 ✅ | `waitq_block_deadline`; readiness notifier and `poll` operation (6.1) | E | SMP suite: `timedwait`, `ioready` |
| M4 ✅ | Process groups and sessions: `setpgid`, `getpgid`, `setsid`, `getsid`; inheritance on fork/spawn; lifetime; pid reuse | F | process-control: checks 200-223 |
| M5 ✅ | Signals: non-tearing `signal_post` (6.3), default-action table, stop and continue, `wait` reports stops, `killpg`, credential-based permission, new numbers, orphaned groups | G | process-control: checks 230-264 |
| M6 ✅ | i386: `machine_user_signal_push/pop`, delivery on trap return, the signal syscalls enabled | H | `test-i386-boot` case `process-signal` (`sigtest`, 26 checks) |
| M7 ✅ | Spawn attributes, applied before the child can run (6.2): argv, inherited descriptors, process group or new session, signal mask | I, D | `spawntest` (27 checks): i386 `process-spawn`, arm64 through `proctest` |

### M1 as built

- `vnode_operations_t` `read`/`write` and `devfs_operations_t` `read`/`write`
  take `ioflags` (`VNODE_IO_NONBLOCK`). `vnode_read_io`/`vnode_write_io` pass
  them; `vnode_read`/`vnode_write` remain as the no-flags form.
- `file_read`/`file_write` take per-call flags, added to the file's open
  flags. `file_is_stream` identifies sockets, character devices and FIFOs.
- `read` asks a stream once; `write` reports a short count rather than an
  error when cut off after moving bytes; `ioctl` accepts descriptors 0-2;
  `read_poll` works on any stream, not only sockets.
- The "not a terminal" status codes move to Phase 2, where they are first
  used.
- No new test: nothing new is reachable yet (the first device that reads is
  the TTY loopback driver in Phase 2, which tests nonblocking reads). The
  change is verified by the existing suites that use these paths.

### M2 as built

- `devfs_unregister_device`, `devfs_set_attributes`; slots are free, named or
  detached, and detached slots are reclaimed lazily (see `doc/vfs/devfs.md`).
- The vnode `open` operation may redirect the open to another vnode;
  `devfs_operations_t.open` does so by filling a `devfs_clone_t`.
- `devfs_self_test` runs at boot on arm64 and i386.

### M3 as built

**Timed waits** (`kern/sched_prism/waitq.h`).
`waitq_block_deadline(queue, seq, interruptible, deadline)` is
`waitq_block_seq` with a deadline and three outcomes: `WAITQ_WOKEN`,
`WAITQ_INTERRUPTED`, `WAITQ_TIMED_OUT`.

- A thread with a deadline is on a global list of timed waiters. The boot
  CPU's scheduler tick walks it and, for each thread past its deadline, takes
  the thread off its wait queue and wakes it. As with signals, only whoever
  unlinks a thread from its queue wakes it, so a deadline and a wakeup racing
  on two CPUs wake the thread exactly once.
- The deadline is armed before the thread joins its queue. If it passes in
  between, the tick finds the thread on no queue, leaves it armed, and ends
  the sleep on the next tick. A wait never ends before its deadline and ends
  within a tick or two after it.
- A wakeup that finds the thread already removed by its deadline wakes
  nobody, so `WAITQ_TIMED_OUT` means "test the condition once more", which the
  header's usage pattern shows.
- Teardown: `sched_thread_terminate` takes the thread off its queue and then
  calls `waitq_timer_cancel`, which goes through the timed-waiter lock whether
  or not the thread is armed. The tick holds that lock from choosing a thread
  to finishing its wakeup, so a thread cannot be reclaimed while a tick is
  still using it, and the list never points at a dead thread.
- Lock order: timed-waiter lock, then a queue's lock, then the scheduler's.
  Nothing takes the timed-waiter lock while holding either of the others.
- Before the scheduler ticks, a timed wait yields instead of sleeping.

**Readiness** (`kern/ioready.h`), the contract of 6.1.

- `ioready_t` is the notifier an object embeds; `ioready_post` delivers to
  the attached watches and kicks the event-wait sleepers; `ioready_destroy`
  hangs up and detaches whatever is still attached.
- `ioready_watch_t` is a listener: the events it asked for, what has fired,
  an optional notify routine. Hangup and error are always delivered.
- The level query is the `poll` operation of `vnode_operations_t` and
  `devfs_operations_t`, reached through `vnode_poll` and `file_poll`; it
  returns the ready mask and the notifier to watch. A vnode or device without
  it is always ready. Sockets answer through `socket_poll`.
- Sockets post when bytes arrive, room appears, a connection is queued or the
  peer closes. That makes the mechanism live today: a thread in the
  event-wait system call now wakes when one of its sockets has something for
  it, with no change to its code.
- A future `poll` attaches one watch per descriptor (obtained from
  `file_poll`), then asks the level, then sleeps on its own wait queue with
  `waitq_block_deadline`. Nothing in a driver, the TTY or the PTY changes.

Tests: the `timedwait` and `ioready` cases of the SMP suite
(`kern/tests/smp_test.c`): timeout not early and not late, wakeup before the
deadline, past deadline, moved sequence, interruption, wakeups racing
deadlines on several CPUs, and a thread terminated mid-wait; watch delivery,
masking, detach, destroy, the event-wait kick, and the levels and edges of a
listener and a connected socket pair.

### M4 as built

`kern/process/pgrp.h` holds the model; the process subsystem owns it and the
terminal will refer to groups and sessions by ID only.

- Every process is in one group, every group in one session. Groups and
  sessions are heap objects named by a pid; membership is their only
  reference count (a group lives while it has members, a session while it has
  groups), so there is no separate count to get out of step.
- A child joins its parent's group in the same process-table critical
  section that makes it visible, for `fork` and for spawn alike (rule 6.2):
  it is never findable, signalable or runnable outside a group. A process the
  kernel starts (PID 1, test processes) leads a new session and group
  instead. The kernel process leads session 0.
- `setpgid` and `setsid` follow POSIX: caller or its child only, same
  session, not a session leader, not a child that has called `exec`
  (`PROC_FLAG_EXECED`), and a new group can only be the one a process names
  after itself. `setsid` is refused to a group leader.
- A zombie stays in its group until reaped; a session keeps existing after
  its leader exits (`s_leader` is cleared at exit). A pid is not reused while
  a group or session is still named by it.
- All state is under the process table lock. Memory is allocated before the
  lock and freed after it.
- System calls 101-104: `getpgid`, `setpgid`, `getsid`, `setsid`, with
  userland wrappers on both architectures.

Tests: `proctest` checks 200-223 (arm64, where `fork` exists), placed before
the capability section so they run regardless of 8.1. They cover leadership
of a kernel-started process, inheritance, creating and rejoining groups, the
refusals, a new session and inheritance into it, a parent building a
two-process job, a group outliving its leader, and a reaped process leaving
its group. Not covered: the `exec` rule (no test helper stays alive after
`exec`), and everything on i386, which has no `fork`; i386 gets its coverage
with spawn attributes in M7.

Moved to M5: orphaned-group detection. It only has an effect on stopped
processes, which do not exist until M5, so it lands and is tested there.

### M5 as built

The rewritten `kern/process/signal.c`; the model is in `signal.h`.

- **A process acts on its own signals.** `signal_take` is the one place a
  signal does anything: the machine-dependent return-to-user path calls it,
  and it parks the thread while the process is stopped, carries out ignore,
  stop and continue itself, and tells the caller only "run this handler" or
  "end the process". arm64 calls it now; i386 calls it from M6.
- **`signal_post` is the non-tearing primitive of 6.3.** It marks a signal
  pending, wakes interruptible sleepers and kicks running threads. It takes
  two spinlocks, never sleeps, and has no path to `proc_exit`. The terminal
  will use it and `signal_post_group` exclusively.
- **`signal_send` / `signal_send_group`** are `kill(2)` and `killpg(2)`. They
  alone may end another process synchronously, because a system call handler
  holds no locks and a target asleep uninterruptibly would otherwise never
  die. They check permission; `signal_post` does not.
- **Stop is self-parking**, on a per-process wait queue at the kernel
  boundary. No scheduler change was needed. A `SIGCONT` counter cancels a
  stop that a concurrent continue has overtaken.
- **Orphaned groups** are computed on demand from the parent links (no
  counters to keep in step): terminal stops are discarded in one, and an exit
  that orphans a group with stopped members sends it `SIGHUP` then `SIGCONT`.
- **Permission** follows credentials instead of ancestry. This changes one
  existing test expectation (check 58: a process may now signal its parent
  when both run as the same user).
- New: `SIGTSTP` 20, `SIGTTIN` 21, `SIGTTOU` 22, `SIGWINCH` 28; system calls
  105 `wait_options` and 106 `killpg`.

Known limits: a thread in an *uninterruptible* kernel sleep acts on a posted
signal only when that sleep ends (socket reads and writes sleep that way
today). Cross-user denial of `kill` is not covered by a test, because the
test program cannot change user. On i386 the signal system calls stay
disabled until M6.

### M6 as built

- **Gate split.** `MACHINE_USER_CONTEXT` now means fork and exec only;
  `MACHINE_USER_SIGNALS` covers handler frames and delivery. arm64 sets both;
  i386 sets signals only. No behaviour change on arm64.
- **Handler frames** (`kern/i386/user.c`). A handler is entered as a cdecl
  call nobody made: return address (the restorer), the signal number, then
  the saved registers, `eip`, `eflags`, `uesp` and the mask, placed so the
  stack is ABI-aligned at the handler's first instruction. `sigreturn`
  restores from it, treating every field as untrusted: `eip` and `uesp` must
  be in user space, and only the flags user code owns are taken back.
  Segment registers and floating-point state are not saved.
- **Delivery** (`kern/i386/syscall_trap.c`). `i386_trap_exit`, which every
  path back to ring 3 already passes through, calls `signal_take` for user
  frames. So a posted signal is acted on at the target's next system call
  return or interrupt, and a kick makes that immediate.
- `kill`, `killpg`, `sigaction`, `sigprocmask` and `sigreturn` are enabled on
  i386, and the userland library has its sigreturn trampoline. Activity
  Monitor's Kill and compositor termination use the same gate.

Not done: a processor exception in ring 3 still ends the process directly
on i386 instead of being turned into `SIGSEGV`/`SIGILL`/`SIGFPE` first, so a
handler for those cannot run there yet (arm64 does this).

Tests: `sigtest`, a spawn-based program run by the `process-signal` case of
`test-i386-boot`. In one process: a handler runs once with the right
argument, registers and stack survive it, the signal is blocked during its
handler and unblocked after, blocked signals wait, ignored ones vanish,
`SIGKILL` and `SIGSTOP` cannot be caught. Between a parent and a spawned
child: stop and its report, a caught signal held while stopped, continue
through `killpg`, the handler then running, and a default-fatal signal.

### M7 as built

- **`spawn_attr`** (system call 107, `nxu_spawn_attr_t` in
  `kern/syscall/syscall_defs.h`): arguments, a list of descriptors to
  inherit (child number ← parent number), a process group to start in (a new
  one, an existing one, or a new session), and a signal mask. Fixed-width
  fields, 112 bytes, identical on both architectures.
- **Rule 6.2 holds by construction.** Group and session placement is an
  argument of process creation (`proc_create_user_in`): it is decided under
  the process table lock before the process is on any list, so a refusal has
  nothing to undo and success makes the process visible already in its
  group. Arguments, mask and descriptors are applied by
  `loader_configure_process` while the process is an embryo with no runnable
  thread. Only then is it started.
- **A failed spawn leaves nothing.** `proc_abort_embryo` takes a never-run
  process apart completely. (Before this, a spawn that failed after creation
  leaked the embryo: `proc_exit` refuses embryos, so
  `loader_discard_process` did nothing.)
- **One entry convention.** Every process, spawned or reached by `exec`, on
  either architecture, starts with its stack pointer at `argc`, `argv`
  above it, in its own word size; a process given no arguments finds
  `argc` 0. Both `crt0` stubs read that. The dynamic runtime enters an
  application the same way (`dyr_enter`) instead of calling its `_start`.
- **`socketpair`** (system call 108) and `filedesc_install_at`.
- **`nxu_spawn_io` is now portable** (`frameworks/lib/spawn_io.c`): a socket
  pair, one end inherited as the child's descriptors 0 to 2, arguments
  through `spawn_attr`. The connect-back handshake in `crt0`
  (`spawn_io_i386.c`, `NXU_SPAWN_IO_ENTRY`) is gone, and `su` and `sudo`
  build on arm64 again.
- Dispositions are not inherited across spawn (a new process starts with
  every signal at its default), so there is no "reset to default" attribute.
- Left for Phase 5, where the terminal exists: making the child's session
  take a controlling terminal, and putting its group in the foreground, as
  spawn attributes.

Not runtime-verified: the dynamic runtime's new entry on i386. It is
verified on arm64 (WindowServer, a dynamically linked service, starts and
serves), and the i386 build links, but the i386 programs that use it only
run in the desktop boot, which needs a display.

A disk built before this change holds programs with the old entry stub; they
start, but without arguments. Rebuild the disk image (for the i386 desktop:
remove `ssd.img`, `system-template.img` and `disk.img` first).

### Verification state

After M7 (2026-11-12):

- i386: `test-i386-boot` cases `bootd`, `process-thread`, `process-ipc`,
  `process-socket`, `process-signal` and the new `process-spawn` pass;
  `test-i386-threads`, `-fs`, `-userland` pass.
- arm64: `default`, `ipc-process`, `thread-process`, `socket-process` pass.
  `process-control` passes everything up to check 88 (8.1), now including
  `exec` with the new stack layout and `spawntest`. `everything` brings the
  desktop up and fails only on that same check. `smp` and `sound` last
  passed after M3.
- Known, unrelated to this work:
  - `process-control` check 88: see 8.1.
  - The `smp` row stalls intermittently on unmodified `main` as well: see 8.2.
  - `test-i386-boot` case `kernel` fails on two missing log lines
    (`i386_init_kernel: ramfs mounted at /...` and
    `i386_init_kernel_selftest: ...`): uncommitted edits to
    `kern/i386/kernel_init.c` renamed those log prefixes. The case passed
    before those edits.
  - `test-i386-sound` fails two cases on a missing log line
    (`virtio_snd_attach: VirtIO Sound device found`). Playback and the
    recording still verify. The line changed in uncommitted
    `drivers/virtio/virtio_sound.c` work on the boot loader's chime, which
    prints a different message on that path; the suite passed before those
    edits.

**Phase 2: TTY core.** Object, refcount, driver interface, input/output
queues, open/close/read/write/ioctl through devfs, a loopback test driver.

### Phase 2 as built

`kern/tty/tty.h` (the driver interface and the model), `tty_internal.h`
(`struct tty`), `tty.c` (lifetime, queues, the devfs file operations),
`tty_ioctl.c`; ABI in `kern/syscall/termios_defs.h`.

- **Object.** A heap `struct tty` with one irqsave spinlock, two 4096-byte
  rings, three wait queues (readers, writers, open/close transitions) and an
  `ioready_t`. Bytes pass through unchanged; there is no line discipline yet.
- **References.** The driver's (from `tty_create`), the name's
  (`tty_register` to `tty_unregister`), one per open file, transient ones.
  At zero the driver's `release` runs and the memory goes. `tty_count()`
  makes a leak visible to tests.
- **Driver interface** (BSD `t_oproc` style): `open` on the first open,
  `close` on the last, `start` after every write (the driver pulls with
  `tty_output_take`), `unthrottle` once a read makes room in an input queue
  that refused bytes or filled, `ioctl` for requests the core does not
  handle, `release`. All called with no terminal lock held; none starts after
  `tty_detach`. Input is pushed with `tty_input`. (Phase 3 changed this: it
  runs the line discipline, can start the driver for echo and is
  thread-context only.)
- **Open and close.** The first open and the last close run the driver with
  a transition flag set; an open that arrives meanwhile sleeps
  (interruptibly) until it clears. A refused first open leaves the terminal
  closed. The last close flushes both queues.
- **Read and write.** A read waits (interruptibly) for at least one byte and
  returns what is queued, up to the size asked; a write queues what fits,
  starts the driver, and waits for room for the rest, reporting a short
  count if cut off after moving bytes. Both honour `VNODE_IO_NONBLOCK`.
- **Hangup and detach.** `tty_hangup`: reads drain then return end of file,
  writes fail with an I/O error, new opens fail with `NO_DEVICE`, readiness
  reports `HANGUP`; cleared by the last close. `tty_detach` is the same and
  permanent: the driver is going away.
- **ioctl.** `NXU_FIONREAD` (`'f'` 127) and `NXU_TIOCOUTQ` (`'t'` 115), as
  `uint32_t`. Anything else goes to the driver, and fails with `NOT_TTY` if
  the driver does not know it either.
- **Readiness.** `poll` reports readable (input, or hung up), writable (room
  and the line up) and hangup; input posts `READ`, `tty_output_take` posts
  `WRITE`, hangup posts all three.

Changes to shared code:

- New status codes `VFS_STATUS_NOT_TTY` and `VFS_STATUS_NO_DEVICE`, system
  call errors 19 `NXU_SYS_E_NOT_TTY` and 20 `NXU_SYS_E_NO_DEVICE` (and their
  names in `RecoveryServices.c`). An object with no ioctl at all (a regular
  file, a directory, a socket, a device without the operation) now answers
  `NOT_TTY` instead of `NOT_SUPPORTED`; nothing depended on the old answer.
- devfs: `devfs_register_device_owned` registers with owner, group and mode
  in one step, so a terminal is never visible with the default 0666. An open
  through a vnode whose name was unregistered after the lookup now fails with
  `NOT_FOUND` without calling the driver, which is what lets a driver free
  its context once the name is gone and its files are closed.
- `tty_init()` runs on both architectures right after devfs is mounted and
  self-tested.

Test: `kern/tests/tty_test.c`, a loopback driver behind `/dev/ttyloop-test`
used through the VFS, with kernel threads for the parts that sleep. It
covers attributes and a duplicate name, a refused first open, driver
open/close counts, would-block, partial reads, readiness levels and edges,
both ioctls, `NOT_TTY` for an unknown request and for a directory, a blocked
reader woken by input, a full output queue (short write, would-block), a
13 KiB stream through both full queues intact and in order with the
unthrottle path taken, flushing at the last close, hangup (sleeping reader
gets end of file, write fails, open gets `NO_DEVICE`, reopen after the last
close), and unregister plus detach (stale vnode refused, open files keep
working, the terminal is freed after its last file). Run as the arm64 kernel
test `tty` (`make test TEST=tty`, a `make check` row) and the i386
`test-i386-boot` case `tty` (`process-test=tty`).

Verified 2026-11-12: arm64 rows `tty`, `default`, `sound` pass;
`test-i386-boot` passes every case including `tty`, except `kernel` (the
known log-prefix failure, section 7 "Verification state"); x86_64 builds.

Not covered: input from an interrupt handler (no driver produces any yet),
an interrupted read or write (kernel threads take no signals; covered once
programs use terminals), and SMP contention (the threads share the boot
CPU, as the VFS requires; the stress case is Phase 8).

**Phase 3: line discipline.** Interface plus the default discipline as pure
code: canonical and raw input, `VMIN`/`VTIME`, echo, special characters, input
and output flags. Host-tested under ASan.

### Phase 3 as built

`kern/tty/n_tty.h`, `n_tty.c` (pure: no locks, no sleeping, no allocation,
nothing but the ABI header), the termios ABI in
`kern/syscall/termios_defs.h`, and the core rewired around it in `tty.c`.

- **ABI.** `struct nxu_termios` as decided in section 5: Darwin's layout,
  flag bits and `c_cc` slots with 32-bit fields, 44 bytes (a static
  assertion holds it), `NXU_POSIX_VDISABLE` 0xff. No request numbers yet;
  those are Phase 4.
- **Input** (`n_tty_receive`): `ISTRIP`, `IGNCR`, `ICRNL`, `INLCR`; `IXON`
  with `VSTOP`, `VSTART`, `IXANY`; `ISIG` with `VINTR`, `VQUIT`, `VSUSP`
  (flush both ways unless `NOFLSH`, echo, ask for the signal); with
  `IEXTEN`, `VLNEXT`, `VDISCARD` (`FLUSHO`), `VWERASE`, `VREPRINT`;
  canonical editing with `VERASE` and `VKILL`, lines ended by newline,
  `VEOL`, `VEOL2`, `VEOF`. A canonical read returns at most one line; a line
  of only `VEOF` reads as end of file.
- **Echo**: `ECHO`, `ECHOE`, `ECHOK`, `ECHOKE`, `ECHONL`, `ECHOCTL`. Erasing
  replays the line's echo widths from the column where it began, so a tab or
  a `^X` is rubbed out exactly; with `IUTF8` an erase takes a whole UTF-8
  sequence. Echo that does not fit in the output queue is dropped.
- **Output** (`n_tty_write`, and echo): `OPOST` with `ONLCR`, `OCRNL`,
  `ONOCR`, `ONLRET`, `OXTABS`, `ONOEOT`, keeping the cursor column.
- **Room.** The input ring is the terminal's input queue (4096 bytes plus a
  delimiter bitmap). Canonical mode keeps a slot for the delimiter and drops
  bytes past a full line (bell under `IMAXBEL`) rather than refuse them;
  raw mode refuses and the driver is unthrottled later.
- **Core.** The discipline runs under the terminal lock; its hooks append to
  the output queue and record what to do after the unlock (start the driver,
  post readiness). Reads apply `VMIN`/`VTIME` with `waitq_block_deadline`:
  from the start of the read when `VMIN` is 0, from the last byte otherwise.
  `VSTOP` stops `tty_output_take`. `tty_get_termios`/`tty_set_termios` (kernel
  only until Phase 4), `tty_termios_defaults` (Darwin's `ttydefaults` plus
  `IUTF8`, which every terminal starts with).
- **`tty_input` is now thread-context only**, a change from Phase 2: echo
  can call the driver's `start` before it returns. Phase 7's UART receive
  was already planned to go through a kernel thread, so nothing is lost. A
  driver must not hold a lock that its `start` takes while it calls
  `tty_input`.
- **Signals.** The signal characters record the signal in the terminal
  (`tty_take_signals`). Delivery to the foreground process group is
  Phase 5, which is where a terminal gets one.

Not handled, by decision: parity, framing and break (`IGNBRK`, `BRKINT`,
`IGNPAR`, `PARMRK`, `INPCK`) until a driver reports them (Phase 7);
`VSTATUS` (there is no `SIGINFO`) and `VDSUSP` (a read-time stop, job
control), both disabled in the defaults; `ALTWERASE`, `ECHOPRT`, `PENDIN`,
`EXTPROC`. Their bits are accepted and ignored. A `VEOF` still unread when
canonical mode is switched off reads as byte 0xff.

Tests:

- `make test-tty-host` (`tools/tty`, in `CHECK_HOST_TARGETS`): every rule
  above as a scripted case against a fake terminal (output room, flushes,
  signals, flow), then 300000 random steps (bytes biased to the special ones,
  reads, writes, random termios, random output room) checking the ring's
  invariants after each. See 8.4 for the sanitizers on this machine.
- `tty_test` (kernel, arm64 row `tty` and i386 case `tty`): the Phase 2
  cases now run the terminal raw; new cases type through the real core with
  a capture driver: an edited line and its echo, `VEOF`, `^C` (input and
  output flushed, `^C` echoed, `SIGINT` recorded), a password typed with echo
  off, `ONLCR`, `^S`/`^Q`, and the four `VMIN`/`VTIME` combinations timed
  by a reader that really sleeps (`0 0` at once; `0 2` about 0.2 s, or at
  the first byte; `3 0` not before the third byte; `4 1` 0.1 s after the
  first byte).

**Phase 4: termios.** ABI header, `TIOCGETA`/`TIOCSETA[W|F]`, flush, drain,
flow, window size; userland `tcgetattr` family and `stty`.

### Phase 4 as built

**ABI** (`kern/syscall/termios_defs.h`). The requests, with Darwin's numbers
and argument types: `TIOCGETA`, `TIOCSETA`, `TIOCSETAW`, `TIOCSETAF`,
`TIOCFLUSH` (`NXU_FREAD`, `NXU_FWRITE`), `TIOCDRAIN`, `TIOCSTOP`,
`TIOCSTART`, `TIOCGWINSZ`, `TIOCSWINSZ`; `struct nxu_winsize` (8 bytes,
asserted). The defaults moved here as `NXU_TTYDEF_*` and `NXU_C*`
(Darwin's `<sys/ttydefaults.h>`), so the kernel's `tty_termios_defaults` and
`stty sane` read the same table.

**Kernel.** `tty_ioctl.c` is a dispatcher; the operations live in `tty.c`:

- `tty_change_termios`: the driver's new `param` operation (BSD's
  `t_param`) is asked first and may refuse with INVALID, leaving the terminal
  unchanged; `TIOCSETAF` discards input in the same locked step as the
  change.
- `tty_drain`: sleeps on the writers' queue until the output queue is empty
  or the line hangs up; interruptible. Stopped output keeps it waiting.
  `TIOCSETAW` and `TIOCSETAF` drain first, so output written under the old
  settings goes out under them.
- `tty_flush`: input (and an unthrottle, if the driver was holding input
  back), output (waking writers and drainers), or both.
- `tty_stop_output`: the same stopped state as `VSTOP`/`VSTART`, so either
  undoes the other.
- `tty_get_winsize`/`tty_set_winsize` (public: the PTY master will set it);
  a real change records `SIGWINCH` for the foreground group, beside the
  signal characters' signals.
- `CREAD` off now discards input as it arrives.

**User space.**

- `frameworks/include/termios.h`: the POSIX names over the ABI, with
  `struct termios` asserted identical to `struct nxu_termios`;
  `frameworks/lib/termios.c` (linked into every program): `tcgetattr`,
  `tcsetattr` (`TCSANOW`, `TCSADRAIN`, `TCSAFLUSH`, and `TCSASOFT`, which
  keeps `c_cflag` and the speeds), `tcdrain`, `tcflush`, `tcflow` (`TCOOFF`,
  `TCOON` by request; `TCIOFF`, `TCION` by writing `VSTOP`/`VSTART`),
  `tcgetwinsize`/`tcsetwinsize` (POSIX 2024), `cfget`/`cfset` speeds,
  `cfmakeraw` (Darwin's), `isatty`. They return 0 or -1 as POSIX says; with
  no C library there is no `errno`, so a caller that needs the reason uses
  `nxu_ioctl`, which returns `-NXU_SYS_E_*`.
- `stty` (CoreServices): BSD's, as macOS ships it. `-a`, `-g`, `-f file`;
  every mode by name; `cs5`..`cs8`; control characters (`^X`, `^?`,
  `undef`); `min`, `time`; speeds; `rows`, `columns`, `size`, `speed`; the
  combinations `raw`, `-raw`/`cooked`, `cbreak`, `-cbreak`, `sane`, `nl`,
  `-nl`, `ek`, `crt`, `newcrt`, `dec`, `evenp`, `oddp`, `parity`, `pass8`,
  `litout` and their negations; `gfmt1:` lines from `-g`. Listings use BSD's
  layout: the short form shows what differs from a new terminal, `-a`
  everything, wrapped at 72 columns.

Not here yet: `tcgetpgrp`, `tcsetpgrp`, `tcgetsid` (Phase 5), `tcsendbreak`
(no driver sends a break), `ttyname`. The background-process rule (a
process outside the foreground group that changes the settings gets
`SIGTTOU`) is Phase 5. Speed 0 (`B0`) does not hang the line up.

**Tests.**

- `tty_test` (kernel), new cases through `vfs_ioctl`: `TIOCSETA`/`TIOCGETA`
  across two descriptors, the driver consulted and its refusal changing
  nothing, `TIOCFLUSH` each way and a bad queue, `TIOCDRAIN` and
  `TIOCSETAW` blocking on a stalled driver and finishing when it resumes
  (and `TIOCSETAW` applying only then), `TIOCSETAF`, `TIOCSTOP`/`TIOCSTART`,
  the window size and `SIGWINCH` only on a real change, `CREAD`.
- `ttytest` (user program), run by `kern/tests/termios_process_test.c` on a
  loopback terminal at `/dev/ttytest`, as an ordinary user: `isatty` on a
  terminal, a file, bad descriptors; the defaults; round trips, `TCSASOFT`,
  unknown actions; raw and canonical bytes through real system calls (a
  canonical read returns exactly one line; `ONLCR` out and `ICRNL` in);
  `tcflush`, `TCSAFLUSH`, `tcdrain`, `tcflow`; the window size; and `stty`
  run as a child with its output read back over a socket pair: `-g` (the
  exact `gfmt1` line) and restoring from it, `raw -echo`, `sane rows cols`,
  the exact short listing and `-a` layout, control characters, an unknown
  operand, standard input that is not a terminal. i386: `test-i386-boot`
  case `termios`; arm64: the `tty` row runs it after `tty_test`.
- The loopback driver moved to `kern/tests/tty_loopback.c`, shared by both.

**Phase 5: job control.** Controlling terminal, foreground group,
`TIOCSCTTY`/`TIOCNOTTY`/`TIOC[GS]PGRP`/`TIOCGSID`, `SIGTTIN`/`SIGTTOU`,
hangup, session-leader exit, `/dev/tty`.

**Phase 6: PTY.** `/dev/ptmx`, slave nodes, lock/grant, hangup on either
side closing; then Terminal.app and nzh move to it and the password prompt
stops echoing. Built: see "Phase 6 as built" below.

**Phase 7: hardware backends.** UART receive, `/dev/console`, serial TTY.

**Phase 8: hardening.** Lock audit, SMP stress, ABI check on both
architectures, limitations written down.

### Phase 6 as built

**Prerequisites.**

- Socket reads and writes sleep interruptibly (`kern/ipc/socket.c`):
  `SOCKET_INTERRUPTED`, mapped to `VFS_STATUS_INTERRUPTED` by `vfs/file.c`,
  so the system call layer restarts them under `SA_RESTART` as it does a
  terminal's. A write that had moved bytes returns the short count.
- **An interruptible sleep no longer starts with a signal pending**
  (`waitq_sleep`, `kern/sched_prism/waitq.c`). Before, a signal posted to a
  thread that was not asleep yet (in user mode between two system calls, or
  on its way to the sleep) was only marked pending, and the sleep that
  followed waited for something else to end it. Callers that knew
  (`wait`, SAP, crash) tested `signal_pending_for` first; the terminal,
  sockets and the PTY did not. The test is now made once, in `waitq_sleep`,
  after the thread is on the queue, so a signal posted after it still finds
  the thread there. Found by `ptytest`: ^C typed right after a job printed
  its prompt was lost about one run in two; 0 in 21 since.
- Ctrl with a letter or `@ [ \ ] ^ _` is folded into the control
  character in the desktop's key events (`ui_service_poll_key`,
  `drivers/video/ui_service_login.c`). No app received ^C before; the
  terminal's `character == 3` test could never fire.

**Kernel** (`kern/tty/pty.c`, `pty.h`).

- `/dev/ptmx` is a cloning device: each open makes a pair, the opener's file
  being the master (a nameless devfs node), the slave registered as
  `/dev/ttysNNN`, owned by the opener's real user, group `tty` (4, added to
  `/etc/group`), mode 0620. At most `PTY_MAX` (32) pairs.
- Darwin's requests (`termios_defs.h`): `TIOCPTYGNAME`, `TIOCPTYGRANT`,
  `TIOCPTYUNLK`. The slave refuses to open (DENIED) until unlocked. The
  master also takes `TIOCGETA`/`TIOCSETA*`, `TIOCFLUSH`, `TIOC[GS]WINSZ`
  (the emulator's window size, and so `SIGWINCH`), and `FIONREAD`; it is no
  one's controlling terminal, so job-control requests are NOT_TTY there.
- The master has a ring of its own, filled from the terminal's output queue
  by the driver's start operation (one mover at a time). That keeps output
  the slave wrote just before its last close, which the terminal discards at
  that close; a full ring leaves output in the terminal's queue, so the
  slave's writers still wait for the master.
- The slave's last close: the master reads what is left, then end of file,
  and readiness reports a hangup. The master's close: the slave is detached
  (its session gets SIGHUP), its name goes, and the pair is freed with the
  slave's last file.

**User space.**

- `<termios.h>`: `posix_openpt`, `grantpt`, `unlockpt`, `ptsname_r`.
- `nxu_spawn_pty` (`spawn_io.c`): a pair, the child leading a new session
  with the slave as descriptors 0-2 and controlling terminal, in the
  foreground, with the window size set. `nxu_spawn_here` (descriptors 0-2
  and group inherited) and `nxu_wait_as_own` (end as the child ended) for
  su and sudo.
- **Terminal.app** spawns nzh with `nxu_spawn_pty` and is only an emulator
  (its socket path and local line editor are gone): keys go to the master
  as a terminal keyboard sends them (Return CR, Backspace DEL, cursor keys
  CSI), and output is drawn with `\r`, `\b`, `\t`, `ESC [ K`, `ESC [ J`
  (0 and 2), `ESC [ H`, `ESC [ n A/B/C/D`. The cursor row is kept apart from
  the bottom row, so it can move up into the screen; it wraps when a
  character is written past the last column, as terminals do.
- **nzh**, on a terminal: a line editor of its own (`line_edit.c`: raw mode
  for the length of a line, history, cursor keys, ^A ^E ^U ^K ^W ^L ^D, Tab
  completion; a line wider than the terminal is redrawn across its rows,
  using the width from `TIOCGWINSZ`; Escape alone is told from a cursor key
  by a tenth-of-a-second `VTIME` read), the terminal's settings put back
  before a command runs; each
  command a job in its own group, in the foreground from its spawn
  (`NXU_SPAWN_SETPGROUP | NXU_SPAWN_SETFG`); waits with `WAIT_UNTRACED`;
  takes the foreground back after, restoring its saved settings if the job
  stopped or was killed; `jobs`, `fg` and `bg`, background jobs reaped (and
  a reader stopped by `SIGTTIN` reported) before each prompt. It ignores
  `SIGQUIT`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU`; ^C at the prompt abandons the
  line. Not on a terminal it reads plain lines and runs commands on its own
  descriptors without job control; the per-child socket relay and in-band
  ^C byte are gone, and both architectures spawn the same way.
- **Passwords**: `nxu_read_password` (`frameworks/lib/auth.c`), Darwin's
  `readpassphrase` in essence: on `/dev/tty`, echo off for the read, the
  settings restored even when ^C, ^\, ^Z, SIGHUP or SIGTERM cuts it short,
  then that signal sent again. su, sudo and passwd use it. su and sudo run
  the shell or command on their own descriptors (`nxu_spawn_here`), in
  their job, ignoring the keyboard's interrupt and quit, and end as it ends;
  the socket relay is gone.
- The arm64 disk (`$(DISK)` in the top-level Makefile) now installs su, sudo
  and passwd setuid root and rewrites every inode's owner to root, as the
  i386 disk always did; before, su could not change identity on arm64.

**Tests.**

- `pty_test` (kernel, `kern/tests/pty_test.c`): pairs and names, slave
  attributes, locked until unlocked, typing and echo, `FIONREAD`, echo off
  from the master, the window size, a blocked master woken, a stream larger
  than the ring and the queue together, the slave's last words after its
  close then end of file and hangup, the master's close hanging the slave
  up, every pair freed. i386: `test-i386-boot` case `pty`; arm64: the `tty`
  row.
- `ptytest` (user program, run by `pty_process_test` as an ordinary user):
  starts nzh on a pseudo-terminal and drives the master as Terminal.app does
  (14 checks): a command and its output, a corrected line, ^C to a job, ^Z
  then `fg`, the history, a no-echo read, su refusing a wrong password
  without showing it, ^C at su's prompt leaving echo on, `su user` and
  `whoami` on the same terminal, `bg` with the job stopped again by
  `SIGTTIN`, Escape alone, editing a line wider than the terminal, and
  `exit` giving end of file and status 0. On failure it prints what the
  master read. Passes on i386 (`pty` case) and arm64 (`tty` row).

Desktop: Terminal.app on the PTY checked by hand on the i386 desktop
(2026-11-13): editing keys, history, ^C, ^Z with `fg`/`bg`, wide lines,
silent password prompts in su, sudo and passwd, ^D and `exit`. A desktop
that keeps an `ssd.img` from before the change still runs the old userland;
remove it (and `system-template.img`, `disk.img`) to pick the new one up.

Not done: `TIOCPKT`/`TIOCREMOTE`; `ttyname`; `cmd &` (the parser takes no
operators); ^Z during `log stream`, which runs inside nzh, which ignores
SIGTSTP.

## 8. Issues found on the way, tracked separately

Neither is caused by this work and neither is fixed by it. They are recorded
here so they stay visible.

### 8.1 `unlink` without the write capability goes to SAP (process-control check 88)

**Finding: this is a regression against the documented policy, not an intended
SAP eligibility.** No implementation or test expectation has been changed.

What the code does (`syscall_unlink`, `kern/syscall/syscall.c`):

```c
if (sap_protected_path(canonical) || !syscall_has_caps(NXU_CAP_FS_WRITE))
	status = sap_delete(current_proc(), canonical);
else
	status = vfs_unlink(canonical);
```

So a caller *without* `NXU_CAP_FS_WRITE` is not denied: its request is
submitted to the Secure Anchor prompt, for any path under `/Volumes/mainOS`.

Evidence that this is not the policy:

- `doc/sap.md` states the policy exactly: "`nxu_sap_delete(path)` explicitly
  requests authorization. Ordinary `unlink` under `/Volumes/mainOS/System` or
  `/Volumes/mainOS/Library` also routes through SAP. **Other unlink operations
  retain the existing filesystem capability checks.**" Only the two protected
  trees are eligible; everything else is a capability check.
- The handler implemented exactly that until commit `f2abdf7` ("WIP: userland
  polish", 2026-11-08), whose diff replaced

  ```c
  if (sap_protected_path(canonical))
  	status = sap_delete(...);
  else {
  	if (!syscall_has_caps(NXU_CAP_FS_WRITE)) return syscall_error(SYSCALL_ERROR_DENIED);
  	status = vfs_unlink(canonical);
  }
  ```

  with the form above. The change arrived inside an unrelated work-in-progress
  commit, and neither `doc/sap.md` nor the test was updated with it.
- Every sibling call still denies at once without the capability: `mkdir`,
  `rmdir` (which also denies protected paths outright), and `open` for
  create/truncate/append.
- The capability model in `kern/syscall/syscall_defs.h` lists `unlink` under
  `NXU_CAP_FS_WRITE` and describes capabilities as default-deny.

Why the test sees what it sees: the file `proctest` names does not exist, so
the SAP worker's `vfs_lookup` fails and `unlink` returns "not found" instead
of "denied". No prompt is shown in that case.

Consequences of the current behaviour, for a file that does exist:

- Any process holding no capabilities at all can raise the trusted,
  input-capturing system modal at will, and the modal's wording ("could break
  your entire system") is wrong for an ordinary file.
- SAP serves one request at a time, so such a process can keep it busy and
  make legitimate requests fail with `BUSY`.
- The explicit `nxu_sap_delete` call already exists for a program that wants
  to ask; routing plain `unlink` there removes the difference between asking
  and being refused.

Recommendation (not applied): restore the pre-`f2abdf7` branch, so a missing
capability is an immediate denial and only the protected trees reach SAP. If
the Terminal's `rm` was the reason for the change (the shell holds no write
capability), the shell should call `nxu_sap_delete` itself when `unlink` is
denied and the user asked for it. Check 88 stays as written and stays failing
until this is decided.

The job-control tests in `proctest` run before the capability section, so
they do not depend on the outcome.

### 8.2 Intermittent stall of the `smp` test row

The `smp` kernel test sometimes stops making progress, with no failure
message, in a different existing case each time (`pinned`, `ipi`, `affinity`,
`stress` have all been seen). It reproduces on unmodified `main` (2 of 5 runs
on 2026-11-11, QEMU TCG, 8 CPUs online) at about the same rate as on this
branch, so it predates the branch. The suite's own per-case timeouts do not
fire, which means the coordinating thread itself stops running: a lost
wakeup or a scheduler deadlock rather than a slow test.

Not investigated further here. Until it is, a single failed `smp` run is not
evidence about a change; rerun it. Suggested starting points: capture a stuck
run with the QEMU monitor (`info registers` on every CPU) to see whether the
CPUs are idle or spinning on a lock, and check `sched_yield` on the boot CPU,
which every stalled case has in common through `wait_for`.

### 8.3 A pointer movement during boot could hang the kernel (fixed)

Reported on the i386 desktop (2026-11-12): moving the trackpad in the QEMU
window during boot stopped it right after `mouse_attach`, with no message.

Cause: `virtio_input_attach` set `DRIVER_OK`, with its event buffers posted,
before registering its interrupt handler, and the handler ignored
interrupts until a flag set after registration. The mouse shares IRQ 10, a
level-triggered line, with the GPU, which is attached earlier, so the line was
already unmasked. An event in that window raised the line; only the GPU's
handler ran, found nothing of its own and did not acknowledge it; and since
`irq_dispatch` counts any handler that ran as "handled", nothing was logged.
The line, still asserted, interrupted again the moment it was dismissed, and
CPU 0 never left the interrupt. The window is wide on the desktop because
each log line is drawn on screen.

Reproduced with QEMU's `mouse_move` (after `mouse_set` to the virtio mouse;
QEMU sends to the PS/2 mouse by default) and a temporary 3 s pause after
`DRIVER_OK`: the original order never left the pause, the fixed order did and
booted.

Fix (`drivers/virtio`): the handler is bound before `DRIVER_OK` and is ready
to acknowledge by then, in `virtio_input`, `virtio_net` (received frames
arrive unprompted the same way) and `virtio_gpu` (a host display change);
`virtio_sound` already did this. `virtio_device_irq_detach` undoes the
binding when an attach fails later. The rule is written at
`virtio_device_irq_attach`.

Not done: `irq_dispatch` cannot tell an unclaimed interrupt from a handled
one, because handlers return nothing, so any future bug of this shape is
equally silent. Linux's "nobody cared" check (count unclaimed interrupts on a
line, disable the line with a message) would need every handler to report
whether the interrupt was its own.

### 8.4 AddressSanitizer does not run on this machine

On this host (macOS 26, Apple clang 17, x86_64) every program built with
`-fsanitize=address` dies in `__pthread_init` before `main`, including the
existing `test-audio-host` programs, which therefore fail here for reasons that
are not the code's. `tools/tty/test_host.sh` probes for a working ASan and,
without one, says so and builds with UBSan, bounds checking and stack
protection instead. The audio script was left as it is.

## 9. Known limitations accepted up front

- No general `poll`/`select` (risk 5).
- No fork on i386; job control there is spawn-based.
- VFS entry remains boot-CPU-bound; the TTY's own locking is SMP-safe, but
  terminal syscalls do not run in parallel until the VFS does.

## 10. Where to resume

Written 2026-11-12, at the end of Phase 1, so the work can be picked up
without the conversation that produced it.

### 10.1 State of the tree

- Branch `tty-subsystem`. **Nothing from this project is committed**; all of
  it is in the working tree, mixed with unrelated in-progress work (boot
  loader rename, `boot/uefi`, `drivers/virtio` sound, `kern/i386/kernel_init.c`
  log messages, DiskRoot). Committing needs the two separated.
- Besides the milestones, the tree carries four small changes made along the
  way: `tools/check.sh` (an empty-array expansion that stopped QEMU launching
  under the system bash 3.2), `loader_spawn_as` with the sound and
  process-control tests started as a non-root user, the verbose boot display
  (`kern/boot/splash.c`: console only, white text, no time stamp on screen),
  and three-digit check numbers in `proctest`.

### 10.2 Running the tests on this machine

```sh
mkdir -p BUILD/check-tmp
TMPDIR=$PWD/BUILD/check-tmp NXU_CHECK_KEEP=1 make check CHECK_ONLY="<ids>"
```

- `NXU_CHECK_KEEP=1` keeps the logs; without it they are easy to lose.
- **Do not edit the tree while a check runs**: each row rebuilds, and a
  half-made edit fails every row after it.
- Do not run two checks at once: the emulators starve each other and rows
  time out.
- arm64 rows that matter here: `process-control default ipc-process
  socket-process thread-process smp sound everything`. i386:
  `test-i386-boot test-i386-threads test-i386-fs test-i386-userland
  test-i386-devices`. `make test-i386-boot` alone takes about four minutes.
- Expected failures today, none from this project: `process-control` and
  `everything` at check 88 (8.1); `test-i386-boot` case `kernel` and two
  `test-i386-sound` cases (log lines renamed by uncommitted edits); `smp`
  intermittently (8.2).
- A verbose boot can be screenshotted headless: `qemu-system-i386 -kernel
  BUILD/i386/kernel.elf -append "-v run-seconds=20" -vga none -device
  virtio-gpu-pci -display none -monitor unix:<sock>,server,nowait`, then
  `screendump` through the monitor socket.

### 10.3 Waiting on a decision

1. **Check 88** (8.1): restore the immediate denial for `unlink` without the
   write capability, or keep the SAP routing and change the test.
2. **Signal permission** (M5) changed from "descendants only" to credentials,
   which flipped check 58. Approved as a roadmap item; confirm or ask for the
   old rule alongside.
3. **i386 desktop** has not been run since M7. It needs a rebuilt disk and
   exercises the dynamic runtime's new entry on i386 and Terminal/nzh on the
   kernel-backed `nxu_spawn_io`.
4. Whether to trim Phases 3 and 4 to what the shell and the password prompt
   need, to reach a working terminal sooner (saves an estimated 600-800
   lines on the way to Phase 6).

### 10.4 Conventions the user set for this project

- No phase or milestone wording in source files or comments.
- Comments and file headers in the style of `kern/ipc/*.c`.
- A new `<subsystem>_init()` prints `"<subsystem>_init: initializing
  <subsystem>"` on entry and `"<subsystem>_init: initialized <subsystem>
  successfully"` on success, with `kprintf`. Keep prints out of anything that
  can run in interrupt context (`ioready_post`).
- Tests land with the code; unrelated breakage is reported, not silently
  fixed; root's capability semantics are not bent to suit a test.

### 10.5 Notes for the phases ahead

What Phase 1 built for each, and the choices already made.

**Phase 2, TTY core** (`kern/tty/tty.c`, `tty_ioctl.c`, `tty_internal.h`).

- A `struct tty` is a refcounted heap object with one spinlock (irqsave: a
  UART interrupt will feed it), an input queue and an output queue, a
  `termios`, a window size, an `ioready_t`, two wait queues (readers,
  writers), a driver vtable and a line-discipline vtable.
- It reaches user space through devfs: `devfs_register_device` with `open`,
  `close`, `read`, `write`, `ioctl` and `poll`. `read`/`write` honour
  `VNODE_IO_NONBLOCK`; blocking uses `waitq_block_deadline` with the lock
  dropped (the socket pattern in `kern/ipc/socket.c`).
- Driver operations (`open`, `close`, `output`, `configure`, `flow`) are
  called with the TTY lock dropped. A driver delivers input with one call,
  `tty_input(tty, bytes, count)`, which may be made from interrupt context
  only if it does nothing but queue and schedule; signal generation is
  deferred to thread context (rule 6.3).
- The first driver is a loopback test driver, exercised by a kernel test.
- "Not a terminal" and "no such device" status codes are added here
  (deferred from M1).
- `tty_init()` is the first function the init-logging convention applies to.

**Phase 3, line discipline** (`tty_ldisc.c`, `n_tty.c`).

- Write the default discipline as pure code over plain buffers, with the
  TTY-specific parts behind a small callback table (echo out, signal, wake),
  so it compiles natively and is tested under ASan/UBSan like `tools/btrfs`
  (`make test-...-host`, listed in `CHECK_HOST_TARGETS`).
- Canonical input must return one line per read; M1 already made the `read`
  system call ask a stream exactly once, which is what makes that possible.
- `VTIME` uses `waitq_block_deadline`.

**Phase 4, termios** (`kern/syscall/termios_defs.h`, re-exported as
`frameworks/include/termios.h`).

- ABI as decided in section 5: 32-bit flags, 8-bit control characters,
  `NCCS` 20, 44-byte `struct termios`, requests in `NXU_IOC` group `'t'`.
  `ioctl` already works on descriptors 0-2.
- Userland: `tcgetattr`, `tcsetattr` and the rest as thin wrappers, plus
  `stty`. No libc is introduced.

**Phase 5, job control on the terminal** (`tty_jobctl.c`).

- The TTY stores a session ID and a foreground group ID, not pointers; the
  process subsystem owns the relationships (`kern/process/pgrp.h`).
- Signals go out through `signal_post_group` only, after every TTY lock is
  released. `SIGTTIN`/`SIGTTOU` use `proc_pgrp_is_orphaned` (an orphaned
  background reader gets an I/O error instead of a stop).
- Needs: a per-session controlling-terminal reference (add to
  `struct session`, cleared by `pgrp_leader_exit_locked`'s caller, which
  must then hang the terminal up from `proc_exit` with no locks held);
  `/dev/tty`; and two spawn attributes that were deferred from M7 because
  there was no terminal: take this descriptor as the controlling terminal,
  and put the child's group in the foreground. Both belong in
  `nxu_spawn_attr_t` (bump `size`; the kernel already checks it) and are
  applied in `loader_configure_process`.

**Phase 6, PTY** (`pty.c`).

- `/dev/ptmx` is a cloning device (`devfs_clone_t`, M2): each open makes a
  master with no name and registers its slave as `/dev/ttysNNN` with
  `devfs_register_device`, owned by the opener with mode 0620
  (`devfs_set_attributes`); closing unregisters it.
- The master's `poll` and `ioready_post` are what let Terminal.app sleep in
  the event wait and wake on output; it reads with `nxu_read_poll`, which M1
  generalised from sockets to any stream.
- The switch-over: `nxu_spawn_io` gains a PTY-backed sibling (the child's
  0-2 are the slave, new session, controlling terminal, foreground), nzh
  drops its in-band Ctrl-C byte and its own line reading, and Terminal.app
  drops local echo and line editing. Keep the socket path until the PTY path
  passes its tests.
- Before this phase, make socket sleeps interruptible, or Ctrl-C will not
  reach a process blocked on a socket until the socket wakes it (M5's known
  limit). (Done; see "Phase 6 as built".)

**Phase 7, hardware backends.**

- The UARTs are transmit-only and polled on every architecture. Receive
  needs an interrupt (or a polling kernel thread) feeding a ring, and a
  kernel thread that drains it into `tty_input`, since there is no
  work-queue and signals must be posted from thread context.
- `/dev/console` is a TTY over the UART; `kputc` and the kernel log stay
  independent of it.

**Phase 8, hardening.** Lock-order audit against sections 6.1-6.3, an SMP
stress case in the `smp` suite, an ABI size check on both architectures, and
the limitations list brought up to date. Worth doing first: i386 faults as
signals (M6's gap), so a crash under a shell is reported as a signal.
