# SMP

The arm64 kernel starts every CPU the Device Tree lists, gives each one its own
MLFQ run queue, idle thread and timer, and lets them wake each other with
inter-processor interrupts. Threads of **user processes run on every CPU**, several
threads of one process and threads of different processes at the same time (see
[Userland on many CPUs](#userland-on-many-cpus)). Kernel threads still start on the
boot CPU only, and the system calls that reach the filesystems and drivers run bound
to the boot CPU (see [Boot-CPU compatibility](#boot-cpu-compatibility-for-single-cpu-kernel-code));
what is still not SMP-safe is listed in [What is not SMP-safe](#what-is-not-smp-safe).

Source: `kern/arm64/smp.c`, `kern/arm64/smp_entry.S`, `kern/sched_prism/`,
`kern/ipi.h`, `kern/lock.h`, `kern/cpuset.h`, and for userland `vm/address_space.c`,
`vm/vm_fault.c`, `kern/process/{proc,task,signal}.c`, `kern/syscall/syscall.c`.
Tests: `kern/tests/smp_test.c`, `kern/tests/smp_user_test.c` and
`frameworks/BootDaemons.framework/smptest.c`.

## Initialization sequence

On the boot CPU (CPU 0), in `kern_start_scheduler()`, after the scheduler
objects exist and the boot CPU's timer runs, before it takes its first interrupt:

1. `platform_discover()` has already recorded each `cpu@` node's MPIDR
   (`platform->cpu_mpidr[]`) and the `psci` node's conduit (`hvc` or `smc`).
2. `smp_boot_secondaries()` assigns **logical ids**: CPU 0 is whichever CPU is
   running this (looked up by its `MPIDR_EL1`), the others are numbered in
   Device Tree order. A logical id is never assumed equal to an MPIDR.
3. It builds a private translation root that identity-maps the two pages holding
   the trampoline (`vmm_create_identity_stub`), because a CPU that PSCI starts has
   its MMU off and the first instruction after it turns the MMU on is still fetched
   from a physical address.
4. For each secondary, in turn: `processor_register()` (its `struct processor`),
   `sched_cpu_prepare()` (its idle thread), a 16 KiB boot stack, and a parameter
   block (MAIR, TCR with TTBR0 walks enabled, the trampoline root as TTBR0, TTBR1,
   SCTLR, stack, entry, VBAR, processor pointer) copied from the boot CPU's own live
   registers so every CPU runs the same translation regime. The block is cleaned
   to the point of coherency (`dc civac`, `dsb sy`) because the new CPU reads it
   uncached.
5. `PSCI CPU_ON` (`0xC4000003`) with the trampoline's physical address and the
   block's physical address as context id.
6. The new CPU runs `arm64_secondary_entry` (`smp_entry.S`, in `.text.boot`): masks
   everything, loads the block, programs MAIR/TCR/TTBR0/TTBR1, drops its TLB and
   instruction cache (`tlbi vmalle1`, `ic iallu`, `dsb nsh`, `isb`), writes SCTLR
   (MMU and caches on), installs VBAR and its stack, and branches to
   `smp_secondary_main` at its higher-half address.
7. `smp_secondary_main()`: `TPIDR_EL1` := its processor (so `current_processor()`
   works from now on), TTBR0 disabled again, `gic_init_secondary()` (its
   redistributor, found by matching `GICR_TYPER` affinity, and its CPU interface),
   the timer PPI and the IPI SGIs enabled in its own redistributor, its own
   physical timer armed at the boot CPU's rate, `cpuN: online` printed, and
   `sched_cpu_idle()` publishes it as schedulable and idles.
8. The boot CPU waits (2 s limit) for each CPU to report, then prints
   `SMP: N CPUs online` and `sched: SMP scheduling enabled, N run queues`.

Boot log:

```
SMP: boot CPU0 MPIDR 0x0, 4 CPU(s) in the Device Tree
SMP: starting CPU1 MPIDR 0x1
cpu1: online
...
SMP: 4 CPUs online
sched: SMP scheduling enabled, 4 run queues
```

A CPU that never reports, or has no redistributor, is left out; the others carry on.

## Per-CPU state

`struct processor` (`kern/sched_prism/processor.h`) is the per-CPU object: logical
id (its first word, so `machine_cpu_id()` needs no other knowledge), MPIDR, state,
active/idle/previous thread, the run queue and its lock, tick and switch counters,
the reschedule flag (`preemption_pending`, the `need_resched`). `TPIDR_EL1` points
at it: `current_processor()` is one `mrs`.

`NXU_MAX_CPUS` is 8. Sets of CPUs are `nxu_cpuset_t` (a word array), never a bare
`uint64_t`. Code never assumes the CPU count.

## Locking model

There is no scheduler-wide lock. `kern/sched_prism/sched.c` documents the rules in
full; in short:

| Lock | Protects |
| --- | --- |
| `processor.runq_lock` (one per CPU) | that CPU's run queue and everything queued on it, its active thread, the MLFQ fields of its queued and running threads |
| `thread.sched_lock` (one per thread) | `thread->on_cpu`, `thread->wakeup_deferred` |
| `g_thread_lock` (`thread.c`) | thread state bits, task membership, affinity |
| `waitq.lock` (one per wait queue) | that queue and the threads on it |

Order: `runq_lock(a) -> runq_lock(b > a) -> g_thread_lock`. `sched_lock` is a leaf:
it may be taken under a `runq_lock` (the switch marks its incoming thread), never
the other way round. All are taken with interrupts masked (`nxu_spin_*
_irqsave`). The hot paths (`sched_switch`, `sched_tick`) take only the CPU's own
`runq_lock`; another CPU's is taken only to place or migrate a thread onto it, and
two are taken together only by migration, lowest logical id first.

Spinlocks (`kern/lock.h`) are test-and-test-and-set with acquire on lock and
release on unlock, which is all the ordering ARM64's weak memory model needs for the
data they protect. `nxu_rlock_t` is a recursive, owner-checked variant used for
the memory subsystems and the console (below).

Shared subsystems that are locked: page allocator (`g_pmm_lock`), kernel virtual
memory (`g_vm_kern_lock`), page-table edits (`g_vmm_lock`), heap (`g_heap_lock`),
console (`g_kconsole_lock`, so a `kprintf` is one unbroken line), wait queues, the
thread table. Order among them: heap, vm_kern, vmm tables, pmm; the console is a
leaf. A panic stops the other CPUs (IPI) and breaks a console lock one of them held.

## Thread states and invariants

| State | Meaning | Leaves by |
| --- | --- | --- |
| NEW | created, `TH_SUSP` | `sched_thread_start` |
| RUNNABLE | `TH_RUN`, on exactly one run queue | a CPU chooses it |
| RUNNING | `on_cpu` set, on no queue | yield/preempt: RUNNABLE; block: WAITING; exit: DEAD |
| WAITING | `TH_WAIT`, on a wait queue | wakeup: RUNNABLE |
| DEAD | `TH_TERMINATE` | reaped by the CPU that switched away from it |

Invariants (`sched_validate`, and `sched_validate_all` for the cross-CPU ones):

- A thread is on at most one run queue, only while runnable, never while running.
- `on_cpu` is set from the moment a CPU chooses a thread until that CPU has
  switched off its stack. A wakeup in that window cannot queue the thread (a second
  CPU could resume a context the first is still using): it sets `wakeup_deferred`,
  and the CPU that is switching away queues the thread in `sched_finish_switch`,
  after its context is saved. A thread that yields is handled the same way
  (`previous_requeue`): it is queued from the *incoming* thread's stack, never
  before the switch.
- Only RUNNABLE, queued threads migrate, under both queues' locks. A running
  thread is never moved by editing pointers.
- A CPU's idle thread is never queued and never leaves its CPU. A secondary CPU's
  idle thread *is* its boot context: it idles on its boot stack.

MLFQ policy is unchanged and runs per CPU: four levels, quanta 4/8/16/32 ticks,
demotion on spending a level's allotment, no penalty for blocking, and a boost of
the CPU's own queue every 400 of its ticks.

## Placement, affinity and balancing

- `thread->affinity` is an `nxu_cpuset_t`. `thread_set_affinity()` /
  `thread_get_affinity()` change and read it; idle threads are pinned.
  `sched_thread_can_run_on()` says whether a placement is allowed.
- **Kernel threads start on the boot CPU only** (`processor_default_affinity()`):
  they call the VFS, drivers and other single-CPU code directly. The boot argument
  `sched.affinity=all` widens that default for kernel threads created after boot-args
  parsing (an experiment, not the supported configuration); SMP tests set affinity
  explicitly. The boot thread is always pinned to CPU 0.
- **Threads of user processes start on every CPU**
  (`processor_default_user_affinity()`); `sched.user=boot` puts them back on the
  boot CPU alone. A user thread narrows its own with the `setaffinity` system call
  (`nxu_setaffinity(mask)`), and `nxu_getcpu()` says where it is running.
- `sched_select_cpu()` picks the CPU when a thread becomes runnable: its last CPU
  if that is idle; otherwise the least-loaded CPU it may run on (an idle one if
  any), staying on the last CPU when that is within one queue place of the best.
  Pinned threads and idle last CPUs are answered without scanning.
- `sched_enqueue_on()` queues the thread under the target's lock and sets the
  target's `need_resched` if the thread is better than what it runs (or it is idle),
  sending a reschedule IPI only when the flag was not already set.
- Balancing is pull-style, from a CPU's own tick every 4 ticks: a CPU that is
  idle, or has two fewer threads than the busiest, takes one queued thread from it
  (`sched_migrate_one`, lowest-priority thread that may run here). It scans all
  CPUs, so it is off the wakeup and switch paths.

## Preemption

The timer tick of each CPU charges its own running thread and sets its
`need_resched`. Nothing switches inside the tick: the interrupt's return path
(`exception_dispatch`) enters the scheduler only at a safe point: the interrupted
code was EL0, the CPU's idle thread, or a kernel thread marked `TH_FLAG_PREEMPTIBLE`
(`thread_set_preemptible()`, for code that takes only irqsave locks). Ordinary
kernel threads still switch only where they yield or block.

## IPIs

GICv3 SGIs, vector = `ipi_type_t` (`kern/ipi.h`), sent with `ICC_SGI1R_EL1` after a
`dsb ishst` (whatever the sender wrote for the receiver is visible before the
interrupt), enabled by each CPU in its own redistributor. `IPI_RESCHEDULE` sets the
target's `need_resched` and wakes it from WFI. `IPI_CPU_STOP` halts the CPU (panic).
There is no TLB IPI (next section).

## Memory ordering and TLB maintenance

- Locks: acquire/release, no separate barriers.
- Handoff between CPUs of a thread's context: the switching CPU saves the context,
  then (on the next thread's stack) releases `on_cpu` under `sched_lock` and queues
  the thread under the target's `runq_lock`; the resuming CPU acquires that lock
  before it loads the context. Release/acquire through the run queue lock orders
  the context stores before the loads.
- Trampoline: parameter block cleaned to the PoC (`dc civac`, `dsb sy`) because
  the CPU reads it with caches off; `dsb`/`isb` around each `msr` that changes the
  translation regime; `ic iallu` before its instruction cache is on.
- `TPIDR_EL1` writes are followed by `isb` (only the writing CPU reads it).
- SGI: `dsb ishst` before `ICC_SGI1R_EL1`.
- TLB: every unmap and permission change uses the **inner-shareable** invalidate by
  address, `dsb ish; tlbi vaae1is; dsb ish; isb`, under the page-table lock, so the
  entry is gone from every CPU before the physical page can be reused. That is why
  no TLB IPI exists. Switching TTBR0 flushes only the local CPU (`tlbi vmalle1`,
  `dsb nsh`): the previous space's translations that must go are this CPU's, and a
  broadcast would empty every idle CPU's kernel entries on each process switch.
  `vm_address_space_t.active_cpus` records which CPUs run a space.
  `smp_test` "memory" catches a non-shareable invalidate (verified by
  mutation: it fails with "a CPU read through a stale translation").

## Userland on many CPUs

What had to change so threads of user processes can run anywhere, and how.

### Per-CPU address-space state

Each CPU owns its TTBR0: `struct processor.active_space` is the space loaded in it
(0 when TTBR0 walks are disabled), and nothing else writes it.
`vm_address_space_activate()` installs a space on the calling CPU only
(`dsb`, `msr TTBR0/TCR`, `isb`, `tlbi vmalle1`, `dsb nsh`, `isb`);
`vm_address_space_deactivate()` disables TTBR0 walks on the calling CPU. The
scheduler activates the incoming thread's space on the CPU that is about to run it
(`sched_activate_thread`), and *deactivates* whatever was loaded when the incoming
thread is a kernel thread, so an idle CPU never keeps a dead process's tables
reachable. The old global "active space" and its mirror in `g_vmm` are gone; the
translation-table count is per space. There are no ASIDs: a TTBR0 switch flushes the
switching CPU's TLB, and every change to a live entry invalidates by address on all
CPUs (below), so there is no ASID allocator, reuse or rollover to get wrong.
`vm_address_space_t.active_cpus` records which CPUs have the space loaded.

`current_proc()` is not a variable any more: it is the process of the running thread
(`thread->home_task`, kept until the thread is reaped, so a thread that was killed
while another CPU ran it still knows whose it is until it has left the kernel).

### Address-space lifetime

A space must not be freed while any CPU can still walk it, and a page must not be
reused while any CPU can still translate through an entry for it.

- A CPU adds itself to `active_cpus` and executes a full barrier **before** it
  installs the root, and removes itself only **after** it has installed another
  (or disabled TTBR0) and flushed its own TLB.
- `task_terminate()` terminates every thread of the process, then for each one that
  is not the caller waits until it is off its CPU (`sched_wait_thread_off_cpu`,
  which interrupts the CPU every half millisecond so a thread in user mode reaches
  the exit path). A terminated thread leaves EL0 at its next exception: the return
  path of `exception_handle()` unwinds it into the exit path instead of returning to
  user mode. Only then `vm_address_space_quiesce()` waits for `active_cpus` to
  empty, and only then are the pages and tables freed. If a CPU never leaves, the
  memory is **leaked, not freed**, and a line says so.
- A thread taken off a run queue and then terminated (or held) before it became
  current is given up by `sched_switch` (`sched_abandon`) instead of being run.
- `proc_exit()` claims the exit with `PROC_FLAG_EXITING` under the process lock, then
  does the teardown **without** holding it (holding it would stop the very threads
  being waited for from finishing a system call that needs it). It runs on the boot
  CPU, so two processes killing each other cannot each wait for the other's threads.

### Page faults and the VM

`vm_fault_user()` treats a fault as a hint: by the time it looks another CPU may have
fixed it. "The page is present and allows this access" is success. Otherwise the
region check and the page install are one critical section under `space->lock`
(the page is allocated before the lock is taken), so:

- two CPUs faulting on one page leave exactly one mapping and the loser gives its
  page back (it does not fail, which would have killed the process);
- an `munmap` cannot be followed by a fault that maps a page into the removed region;
- `vm_address_space_cow_break()` copies outside the lock and installs only if the entry
  is still the one it copied from; if not it starts over, and a page another thread
  already made writable counts as done;
- `vm_shm_map_into()` reserves its address range under the space lock and
  `shm_registry_attach()` takes its reference under the registry lock;
- `vm_address_space_unmap_page()` invalidates by address, inner-shareable, whether or
  not the space is loaded anywhere, with the space lock held across the invalidate;
- fork write-protects the parent's pages under its lock and flushes all CPUs.

### Process, signal and thread locking

| Lock | Protects |
| --- | --- |
| `g_proc_lock` | the process table, lists, `p_stat`, `p_flag`, zombie state |
| `proc.p_siglock` | `p_sigact`, `p_sigpending` (a leaf; taken with interrupts masked) |
| `space->lock` | that address space's tables, region list, `mmap_next_va` |
| `ipc_port.ip_lock` | one port's queue, references, active flag |
| `socket.lock`, `ipc_space` lock | as before |
| `waitq.lock` | a wait queue (and its sequence number) |

Order: `g_proc_lock -> p_siglock`, `space->lock -> pmm`, `runq_lock -> g_thread_lock`.
The thread list of a task is walked with references
(`task_first_thread_ref`/`thread_next_task_thread_ref`), never through the raw links.
`syscall_thread_exit` terminates the calling thread first and only then asks whether
any thread is left, so two threads exiting together on two CPUs cannot both conclude
"not the last" and leave the process without an exit. A signal sent to a process kicks
the CPUs running its threads so it is delivered at the next return to user mode.

### Sleep and wakeup: no lost wakeups

`waitq_seq()` / `waitq_block_seq()` (kern/sched_prism/waitq.h) close the window
between "the condition is false" and "I am on the wait queue" without any lock
shared with the waker: the sleeper takes the queue's sequence number before it tests
the condition, and sleeps only if the number has not moved; every wake moves it.
`wait()`, NXPC receive (`ipc_port_wait_prepare`/`ipc_port_wait_seq`) and the socket
loops use it. `waitq_block_unlock()` is the variant for a caller that holds a
condition lock. The scheduler side (a wakeup during a switch-out is deferred to the
CPU switching away) is described above.

`sched_sleep_us()` is a timed sleep built on the same primitive (the boot CPU's tick
wakes all sleepers, so the resolution is one 10 ms tick). Use it instead of a
`sched_yield()` loop to wait: a yielding thread stays on a run queue and counts as
load, which stops the balancer from moving work onto its CPU (the `smp-user` harness
waiting for `smptest` used to cause the rare ring stall this way).

### User-thread migration

A runnable user thread moves like any other: only queued threads migrate, under both
queues' locks. Everything a user thread needs is in the thread and on its own kernel
stack, none of it on a CPU: its EL0 registers are the exception frame on its kernel
stack, the kernel registers `arm64_enter_el0` parked are on the same stack, its
address space is looked up from its task and installed on whatever CPU runs it next,
and signal state is the thread's `sig_blocked` and the process's pending set. FP/SIMD
is not supported: the kernel and userland are built `-mgeneral-regs-only`, the kernel
never enables FP access or saves FP registers, so a thread has no FP context to
migrate (an FP instruction at EL0 is not something NXU handles; if the CPU were to
allow it, its state would be shared by whatever ran on that CPU, as it is on one
CPU). There is no thread-local-storage register (`TPIDR_EL0`) support either. A
thread that is on a CPU (`thread->on_cpu`) is never queued anywhere else.

### Boot-CPU compatibility for single-CPU kernel code

The VFS and filesystems, the drivers (VirtIO block, input, sound, display), the
display and audio system calls, and process creation (spawn, fork, exec, which read
the image through the VFS and copy descriptor tables) have no locking. Instead of
wrapping them in one big lock, a thread that is about to call them moves to the boot
CPU and stays there until it is done (`sched_bind_boot_cpu()` /
`sched_unbind_boot_cpu()`, nesting, tracked in `thread->legacy_depth`): on that CPU
kernel code runs to completion or to an explicit sleep, exactly as before, so no two
threads are in it at once except at a sleep, and device interrupts arrive on the
same CPU. `syscall_dispatch()` does this for every system call not listed in
`syscall_is_cpu_agnostic()` (the default is *bound*: a new system call is safe until
someone audits it), and `proc_exit()` does it itself because it closes descriptors.

Limits: those calls are serialised on one CPU, so the boot CPU is where all file and
device I/O of every process is executed; the move is a scheduling switch, so it must
not be done holding a spinlock; and a thread that is bound cannot use its affinity to
stay off the boot CPU during the call.

## What is not SMP-safe

Not made safe. Threads that use them run on the boot CPU only (kernel threads by
default affinity, user threads by the boot-CPU binding of the system calls that reach
them); interrupts from devices are all routed to CPU 0.

- VFS, ramfs, devfs, ext4 (with JBD2), btrfs: no locking (see the compatibility
  section: reached only from the boot CPU).
- Drivers: VirtIO block (one synchronous request), input, GPU/ramfb, sound; the UART
  driver used outside `kprintf`; `irq_dispatch` tables; the WindowServer and UIService
  in the kernel (they run on the boot thread, pinned to CPU 0).
- Process creation (`spawn`, `fork`, `exec`): boot-CPU bound. `exec` in a process
  with more than one thread does not stop the other threads (unchanged behaviour).
- The descriptor table (`p_fd`) is used only by boot-CPU-bound system calls; a
  process's own threads are serialised there.
- The console history ring is read without the console lock (racy, tolerable).
- Kernel threads other than the tests' own use the single-CPU subsystems directly and
  keep the boot CPU by default.
- `sched.affinity=all` (kernel threads on any CPU) is an experiment: those threads
  call the subsystems above without the boot-CPU binding.
- Exec/process IDs, `p_fd` and the ELF loader assume one process is being created at a
  time (boot-CPU bound, so true).

## Assumptions about the platform

- QEMU `virt`, GICv3 (`gic-version=3`), affinity routing on, redistributors in one
  contiguous region (stride 0x20000, or 0x40000 with VLPIS), `cortex-a72`.
- Firmware provides PSCI (conduit from the Device Tree, `enable-method="psci"`);
  CPUs start at EL1 with the MMU off and interrupts masked.
- `MPIDR_EL1` affinity fields identify a CPU to PSCI and to the GIC; `cpu@` reg
  values equal them.
- The trampoline lies in `.text.boot` within two pages of the image's start.
- All SPIs are routed to CPU 0 (`gic_enable_spi`); PPI 30 is each CPU's own timer.
- At most `NXU_MAX_CPUS` (8) CPUs; more are ignored.
- Caches are coherent between CPUs in the inner-shareable domain, as on QEMU.
- The timer runs at 100 Hz on every CPU and all CPUs read one system counter.

## Building and running

```sh
make -j8 BUILD_ROOT=BUILD CONFIG=smp EXTRA_CFLAGS=-DNXU_SMP_TEST all   # the SMP test kernel
make test TEST=smp                    # builds it and boots it with -smp 4
make check CHECK_ONLY=smp             # headless, pass/fail
```

Any kernel boots SMP with QEMU's `-smp N`:

```sh
qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a72 -smp 4 -m 512M \
    -kernel BUILD/default/kernel.bin -global virtio-mmio.force-legacy=false \
    -drive if=none,format=raw,file=disk.img,id=nxudisk,snapshot=on \
    -device virtio-blk-device,drive=nxudisk -serial stdio -display none -monitor none
```

Add `-append "sched.affinity=all"` to let new kernel threads run on any CPU.

### What the `smp` test checks

| Case | Checks |
| --- | --- |
| `cpus` | every CPU online with an idle thread, unique boot stacks, valid processor state, every CPU's timer ticking |
| `pinned` | one pinned thread per CPU runs on its CPU, and all run at the same time (a barrier that only passes if they overlap) |
| `mlfq` | 3 preemptible CPU-bound threads per CPU: all progress, every CPU runs them, no thread on two CPUs, run-queue invariants hold under churn, demotion happens |
| `wakeups` | 1000 sleep/wake round trips between two CPUs, no lost wakeup |
| `ipi` | waking a thread pinned to an idle (WFI) CPU: median latency well under a tick, reschedule IPIs counted |
| `affinity` | a subset excludes CPU 0; pinning to CPU 0 holds; narrowing a running thread's affinity moves it |
| `balance` | 12 threads queued on CPU 1 with wider affinity: spread over the other CPUs by the balancer, and faster than one CPU |
| `stress` | 4 CPUs contending one lock (exact count); threads created and exiting on every CPU, all reaped |
| `memory` | page allocator, kernel VM and heap hammered from every CPU; a TLB shootdown check that unmaps and remaps a page under a reader's cached translation |
| `seqwait` | 3000 sleep/wake rounds between two CPUs using only the wait-queue sequence number (no lock shared with the waker) |
| `usertlb` | a user page unmapped and a different one mapped at the same address while another CPU holds the old translation in its TLB |
| `vmrace` | two CPUs faulting in pages while the region is unmapped under them: no page may reappear in the removed region, none may leak |
| `cowrace` | two CPUs breaking the same copy-on-write page at once: both succeed, one copy, no leak |

Every case was also checked to fail when its subject is broken (mutations: a
non-shareable `tlbi`, no reschedule IPI, no deferred wakeup, no page allocator lock).

### What `smp-user` checks

`make check CHECK_ONLY=smp-user` (QEMU `-smp 4`, an otherwise idle machine: no bootd,
no boot chime) spawns `smptest`, a user process that runs, at EL0:

| Case | Proves |
| --- | --- |
| 1 threads of one process | a token ring of 4 spinning threads completes in ~0.1 s (it needs all four on a CPU at once; sharing one CPU costs a 40 ms quantum per hand-off), on 4 CPUs |
| 2 processes | the same across 4 forked processes over shared memory |
| 3 address-space isolation | 4 processes use the same virtual addresses with different contents while being forced onto other CPUs 480 times; nothing is seen from another space or lost |
| 4 concurrent page faults | 4 threads fault the same fresh page at the same instant, 192 times; all writes survive, pages are zero-filled |
| 5 shared memory | atomic increments and a published block across CPUs are exact |
| 6 IPC | 400 blocking NXPC round trips between two processes pinned to different CPUs |
| 7 termination | processes killed (SIGKILL) and processes that `exit` while their other threads spin on other CPUs; each is reaped with the right status and leaves nothing behind |
| 8 migration | a computation forced across CPUs (400 moves) gives the same result as unmoved |
| 9 VM stress | map/touch/unmap in 4 threads with a fork in flight, no errors |

The kernel then checks that user threads ran on every CPU, that up to four CPUs ran
them **at the same time** (`sched_user_running_peak()`), that the run-queue invariants
hold and that no pages leaked.

### Mutation checks

Each protection was removed on purpose and the intended test failed (then the change
was reverted): a non-shareable TLB invalidate on user unmap (`usertlb`); a COW break
that loses the race and reports failure (`cowrace`); a fault installed without the
region re-check (`vmrace`); the same without the space lock (`vmrace`); no wait for a
dying process's threads and no address-space quiesce (`smp-user`: SIGSEGV kills and a
panic); no IPC wakeup on enqueue (`smp-user`: the receive hangs); the sequence number
ignored (`seqwait`: a lost wakeup).
