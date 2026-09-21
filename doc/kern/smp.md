# SMP

The arm64 kernel starts every CPU the Device Tree lists, gives each one its own
MLFQ run queue, idle thread and timer, and lets them wake each other with
inter-processor interrupts. Whether *threads* actually run on more than one CPU
is a policy the kernel keeps conservative for now, because most of the kernel was
written for one core: see [What is not SMP-safe](#what-is-not-smp-safe).

Source: `kern/arm64/smp.c`, `kern/arm64/smp_entry.S`, `kern/sched_prism/`,
`kern/ipi.h`, `kern/lock.h`, `kern/cpuset.h`. Test: `kern/tests/smp_test.c`.

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

Order: `runq_lock(a) -> runq_lock(b > a) -> g_thread_lock`. `sched_lock` is a leaf,
never held with a `runq_lock`. All are taken with interrupts masked (`nxu_spin_*
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
- **New threads start on the boot CPU only** (`processor_default_affinity()`),
  and threads of user tasks are restricted to the boot CPU whatever their
  affinity says. Both are deliberate: see the next section. The boot argument
  `sched.affinity=all` widens the default for threads created after boot-args
  parsing (an experiment, not the supported configuration); SMP tests set
  affinity explicitly. The boot thread is always pinned to CPU 0.
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

## What is not SMP-safe

Found in the audit and **not** made safe. Threads that use them run on the boot CPU
only (default affinity; user threads pinned), which keeps their single-CPU
assumptions valid; interrupts from devices are all routed to CPU 0.

- **User address spaces and process identity**: which space is live in TTBR0
  (`g_active_address_space`, `g_vmm.root`), `current_proc()`, the EL0 entry/return
  state parked by `arm64_enter_el0`. One CPU's worth of state. Running user threads
  on secondary CPUs needs these per CPU.
- `vm_map`, `vm_shm`, `vm_fault`, `user_copy`, COW: assume one CPU changes a space.
- Processes, tasks, signals, `syscall`: `proc.c`/`task.c` tables and signal state
  are not locked (only `thread.c` and IPC spaces have their own locks).
- IPC ports and sockets: sleep-then-check patterns that mask interrupts on one CPU
  (`socket.c` enqueues on a wait queue, drops its lock, then blocks). Safe only when
  sleeper and waker share a CPU. `waitq_block_unlock()` is the primitive that fixes
  this for a subsystem that adopts it.
- VFS, ramfs, devfs, ext4 (with JBD2), btrfs: no locking.
- Drivers: VirtIO block (one synchronous request), input, GPU/ramfb, sound; the
  UART driver used outside `kprintf`; `irq_dispatch` tables; the WindowServer and
  UIService in the kernel.
- The console history ring is read without the console lock (racy, tolerable).
- The `libk` allocation-free helpers are fine; `kmalloc` users are fine.

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

Every case was also checked to fail when its subject is broken (mutations: a
non-shareable `tlbi`, no reschedule IPI, no deferred wakeup, no page allocator lock).
