# System observability and SMP resource lifetime

Activity Monitor already exists. Extend it alongside a terminal consumer of
the same kernel statistics interface.

## Existing coverage

- `drivers/video/ui_service_activity.c` exports per-core busy time, per-process
  thread CPU time, physical total/free pages, heap pages, context switches,
  process/thread counts, and process scheduler state through UIService.
- `make test TEST=smp-user` runs user processes on four CPUs and exercises
  cross-CPU IPC, concurrent faults, migration, termination, and concurrent VM
  map/touch/unmap/fork work. Its kernel harness checks per-CPU dispatch and
  physical page recovery with a 32-page tolerance.
- User threads already default to all CPUs (`sched.user=boot` opts out).
  The normal desktop boots four CPUs; `run-console` still boots one.
- SHM explicit withdrawal and attachment cleanup on task termination exist.
  Publications now carry their creator's unique task ID. After the creator's
  threads stop, termination withdraws any abandoned publications. References
  already held by receivers survive. Kernel-owned publications still require
  explicit withdrawal. Kernel tests cover repeated ownership cleanup and
  attachment survival; SMP case 12 exercises 128 creator exits.

## Next implementation slices

1. Extend Activity Monitor to display the additional statistics records below.
   UIService already uses the shared CPU snapshot collector; its process adapter
   uses the synchronized thread snapshot collector. Terminal `top` now consumes
   both the original and additional ABI records.
2. Add fault-resolution outcomes, scheduler queue wait time, and per-device
   VirtIO statistics. Hardware fault/IRQ and aggregate VirtIO counters now exist.
   Distinguish unique physical ownership from the resident mapping counts now exposed.
   Preserve exited-thread CPU accounting.
3. Audit process/thread/IPC destruction and deferred reclamation. Quiescence
   timeout currently deliberately retains memory; replacement must keep task
   and address-space storage alive until remote CPUs release every reference.
   Do not merely free on timeout or increase the timeout.
4. Add configurable thousands-of-process churn and long-duration SMP runs,
   with warm-up, baseline/peak/final page counts and bounded watchdogs. Include
   creator death while receivers remain attached and process death while a
   remote CPU is faulting or operating on its address space.
5. Run WindowServer and applications across CPUs, extend Activity Monitor with
   the new counters, and validate desktop behavior during sustained churn.

Registry IDs still reuse slots and have no capability checks. Generation-tagged
IDs and creator-authorized withdrawal need a separate ABI/IPC review; automatic
creator cleanup does not solve those issues.

## Focused validation

`make check CHECK_ONLY=smp-user CHECK_I386=0` builds and boots the four-CPU
regression in an isolated disk image. Kernel SHM self-tests also run during POST.
This is a bounded regression, not evidence of long-duration stability or a full
destruction audit.

## Statistics ABI v1

`NXU_SYS_STATISTICS` (71), wrapped by `nxu_statistics`, accepts a
`nxu_stats_request_t` from `kern/syscall/statistics.h`. The same fixed-width
layouts are asserted on ARM64 and i386. Set `struct_size`, `version`, `kind`,
`record_size`, `records` (a user buffer address), and `capacity`; zero `reserved`.
The syscall returns the number copied and fills `count` and `total`. Query with
capacity zero, then allocate space, or use bounded arrays and detect truncation
with `count < total`. Table membership may change between calls.

Kinds are SYSTEM (physical total/free and heap pages, page size, CPU/process/
thread counts, timestamp), CPU (busy microseconds, context switches, dispatches,
preemptions, quantum expirations, queue depth, online state), PROCESS (identity,
parent PID, state/flags, name, live non-idle thread count and CPU time), and THREAD
(identity, task identity, CPU time, last CPU, MLFQ level, idle/running/waiting flags).
System thread counts include allocated idle threads and terminated threads awaiting
reaping; thread tables contain active threads. Idle thread CPU time is zero. CPU records include registered slots,
including offline CPUs; inspect `online`. Times use the monotonic machine timer.
Compute CPU usage from differences between samples, not cumulative totals.

Counters and CPU-time pairs are read under their writers' locks, including on
32-bit i386. Queue locks and thread accounting locks are released before user
memory copies. Each caller owns its snapshot buffers. System fields and table
joins are independently sampled, not one atomic system-wide snapshot. Process
CPU time currently sums live threads and can decrease when a thread exits; v1
makes no lifetime-total claim. Process state is the process table state, while
thread scheduling flags are best-effort observations. Additional record kinds expose memory and events without changing these layouts.

Invalid version, request size, record size, kind, or reserved fields return
`-NXU_SYS_E_INVALID_ARGUMENT`; inaccessible request/output buffers return
`-NXU_SYS_E_BAD_ADDRESS`, allocation failure returns `-NXU_SYS_E_NO_MEMORY`.
Failed copies may leave partial output; use it only on success. No capability
is required, matching the existing system-wide Activity Monitor visibility.

SMP case 13 checks malformed requests, invalid addresses, count-only queries,
truncation, identity, and simultaneous per-CPU observers checking monotonic CPU
and thread counters. The i386 ring-3 test verifies the same syscall/layout.

```c
nxu_stats_cpu_t cpus[8];
nxu_stats_request_t request = {
    .struct_size = sizeof(request), .version = NXU_STATS_VERSION,
    .kind = NXU_STATS_CPU, .record_size = sizeof(cpus[0]),
    .records = (uint64_t)(uintptr_t)cpus, .capacity = 8
};
int64_t count = nxu_statistics(&request);
/* On success, cpus[0..count) are valid; request.total detects truncation. */
```

## Memory and event records

V1 also supports PROCESS_MEMORY (5), CPU_EVENTS (6), VIRTIO (7), and
THREAD_EVENTS (8); use the corresponding record size from the shared header.

PROCESS_MEMORY joins on process unique ID. Resident pages count present user
leaf mappings, including the ELF image, populated stacks/anonymous regions,
SHM, and COW mappings. Shared pages count mappings in the dedicated SHM window;
COW pages belong to the resident total but are not classified as SHM. Aliases
and sharing count once per mapping in each process. Anonymous reservation
pages include both populated and unpopulated pages and exclude ELF and SHM.
Page-table pages are separate overhead. These values must not be added together
as if they were disjoint physical ownership. The kernel process has the
MEMORY_KERNEL flag and zero user-space counts; system/heap statistics describe
its allocations instead.

Mapping counts are maintained on install, unmap, fork, and page release under
the map lock. Memory snapshots briefly bind to CPU 0 to serialize with the
existing exec/exit paths that replace/destroy entire map structures; remote
faults and mmap/munmap are serialized by the map lock. No page tables or task
pointers escape to observers.

CPU_EVENTS counts hardware instruction/data page faults and interrupt entries,
including timer and IPI interrupts. CPU event counters use separate per-CPU locks to avoid a shared lock on every
fault/interrupt. Events before processor bootstrap are attributed to CPU 0.
Spurious ARM64 INTIDs and the x86 APIC
spurious vector are excluded. x86 PIC spurious entries and synthetic test IRQs
may still count. THREAD_EVENTS counts user-mode hardware faults per live thread.
PROCESS_MEMORY.page_faults accumulates user faults over the task lifetime,
including exited threads, and survives exec. Kernel faults contribute only to
CPU events; software user-copy fault resolution contributes no hardware trap.
Fault counts include failed/protection faults as well as resolved faults.

VIRTIO aggregates split-queue submissions, consumed completions, device-reported
used lengths, invalid completion IDs, rejected submissions, descriptor depletion,
and queues currently initialized. Used lengths are not disk/network payload
bytes. Rejection/depletion includes API/test failures and is not a device-error
rate. Counters include the isolated boot test queue, so boot self-tests contribute
a small, known amount of traffic. Drivers retain their existing completion-ID
validation behavior.

## Terminal top

Both architectures build `top` and stage it at
`/System/Library/CoreServices/top` (under `/disk` in the mounted development image).
It samples once per second, displays per-core CPU usage/queue depth/events,
physical memory, VirtIO totals, and processes sorted by CPU usage. Rows join
memory data by unique ID. CPU usage counts 100% per core, so a process using
several cores can exceed 100%. Decreasing live-thread totals are treated as zero
delta, and new processes need a second sample for a CPU percentage. Missing
memory records display `-`; table truncation is reported.

ARM64 exec accepts `top --once --plain`: `--once` takes two samples one second
apart and exits, while `--plain` omits terminal clearing escapes. Normal spawn
runs continuously. i386 has no argv startup convention; boot arguments
`top.once` and `top.plain` provide the same controls on both architectures.
Terminate continuous runs through process signals. Process names are sanitized
before terminal output.

SMP case 14 verifies reservation/population/unmap and SHM counters, plus
per-process/per-thread hardware fault attribution and live IRQ/VirtIO traffic.
Case 15 forks and execs the real one-shot `top`. Kernel statistics self-tests
exercise queue error paths, completion accounting, and backing-page recovery.
