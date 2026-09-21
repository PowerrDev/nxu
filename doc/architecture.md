# NXU Architecture

## Overview

NXU is a freestanding AArch64 kernel for QEMU `virt`. The kernel executes at
EL1 from a permanent higher-half `TTBR1_EL1` mapping and schedules EL0 threads
through per-task `TTBR0_EL1` address spaces.

The current machine is deliberately single-core, but process, task, thread,
interrupt, device, VFS and filesystem ownership are already separated so those
subsystems do not depend on bootstrap-only state.

## Execution environment

| Property | Current value |
| --- | --- |
| Architecture | AArch64 |
| Kernel | EL1 |
| Userspace | EL0 |
| Machine | QEMU `virt`, GICv3 |
| CPU | `cortex-a72` |
| CPUs | 1 |
| RAM | 512 MiB |
| Physical kernel load | `0x40080000` |
| Kernel VMA | `0xFFFFFF8040080000` |
| Kernel translation | `TTBR1_EL1` |
| User translation | per-task `TTBR0_EL1` |
| Kernel stacks | per-thread 16 KiB stacks |
| Scheduler | fixed-priority, timer-preemptive EL0 |
| C environment | freestanding, general registers only |

Early assembly executes through the physical load alias. The MMU transition
installs the higher-half kernel and direct map, moves execution permanently to
TTBR1, and removes the lower kernel alias before userspace is scheduled.

## Object model

```text
proc
├── identity / PID / hierarchy / exit state
├── filedesc
└── task
    ├── vm_address_space
    └── threads
        ├── AArch64 machine state
        ├── EL1 kernel stack
        └── scheduler state
```

`proc` owns BSD-style process identity and file descriptors. `task` owns the
execution address space and thread collection. `thread` owns CPU execution
state and scheduling state. User PC/SP belong to the thread, not the process.

## Scheduling

CPU 0 owns one processor object and one 64-priority run queue. Equal-priority
threads are selected FIFO/round-robin. The idle thread is a fallback and is not
kept on the normal run queue.

AArch64 context switching preserves `x19-x28`, frame pointer, link register and
SP. Scheduler dispatch installs the selected task's TTBR0; kernel threads run
with the user translation regime deactivated.

The physical timer runs at 100 Hz. A four-tick quantum preempts EL0 and can
switch to another runnable thread. Ordinary EL1 kernel preemption remains
deferred until kernel critical sections have complete preemption-disable
accounting.

## Memory

```text
PMM
 ↓
translation tables / VMM
 ↓
TTBR1 higher-half direct map
 ↓
vm_kern virtual arena
 ↓
heap
```

The PMM owns physical 4 KiB pages. The VMM owns AArch64 stage-1 mappings.
`vm_kern` owns reserved kernel virtual ranges, and the heap provides byte-sized
kernel allocation.

User address spaces are independent TTBR0 roots. User-copy helpers translate
and validate permissions page-by-page instead of dereferencing arbitrary EL0
pointers directly.

## Interrupts and devices

Exception entry saves a complete trap frame before the machine-independent IRQ
layer dispatches the acknowledged GIC INTID.

```text
GICv3
  │
  ├── physical timer PPI
  │
  └── VirtIO MMIO SPIs
          │
          ├── VirtIO Input
          │     ├── keyboard
          │     └── mouse
          │
          └── VirtIO Block
```

VirtIO transport addresses and interrupt IDs are discovered from the Device
Tree. The VirtIO core owns modern MMIO negotiation and split virtqueues; class
drivers own device semantics.

Keyboard and mouse drivers publish transport-neutral input events. VirtIO Block
publishes a synchronous `block_device_t` interface so filesystems never depend
on VirtIO descriptors.

## VFS and storage

```text
proc filedesc
     │
     ▼
   file
     │
     ▼
   vnode
     │
     ▼
    VFS
   ┌─┴───────────────┐
   │                 │
 ramfs             ext4
                     │
                    JBD2
                     │
              block_device_t
                     │
                VirtIO Block
```

VFS owns filesystem registration, mounts, vnodes, pathname traversal, global
open-file objects and per-process descriptor tables. Filesystem implementations
attach through vnode operation vectors.

`ramfs` is the in-memory validation filesystem mounted at `/`. Writable ext4 is
mounted at `/disk` and consumes only the block-device interface.

The current ext4 profile validates CRC32C metadata checksums for the primary
superblock, group descriptors, allocation bitmaps, inodes, directories and
external extent blocks. Writable metadata is staged as full filesystem blocks
inside a JBD2 transaction. Regular file data is written before the metadata
commit, giving the current implementation ordered-data semantics.

JBD2 uses checksum-v3 descriptor tags and a commit record. Normal commits are
checkpointed immediately. Mount recovery can replay the one committed,
uncheckpointed NXU transaction left by the deterministic crash test.

## Initialization boundary

Kernel initialization establishes memory management, the heap, process/VFS
state, threads and scheduler before device interrupts are enabled. VirtIO input
and block devices are attached while IRQs are masked. The ext4 mount and journal
recovery complete before the first EL0 thread is dispatched.

The scheduler then owns TTBR0 activation and PID 1 enters EL0 through the normal
thread dispatch path.

## Current limitations

- SMP: every CPU comes up and schedules its own kernel threads, but threads
  run on the boot CPU only by default and user threads never leave it; most
  subsystems are not SMP-safe. See [SMP](kern/smp.md).
- Ordinary EL1 kernel preemption is opt-in per thread (`TH_FLAG_PREEMPTIBLE`).
- The bootstrap userspace program is still embedded machine code rather than an
  ELF loaded from VFS.
- VirtIO Block is synchronous and uses one request at a time.
- ext4 write-side extent insertion is limited to the current compact extent
  foundation; general tree growth/rebalancing is not implemented.
- JBD2 immediately checkpoints normal transactions and recovers only the
  single pending transaction shape emitted by NXU. Revoke emission, arbitrary
  multi-transaction log scanning and asynchronous checkpointing are not yet
  implemented.
- There is no networking, dynamic linker, general userspace libc or graphical
  compositor yet.

## Related documentation

- [Boot](boot.md)
- [Higher-half kernel](vm/higher-half-kernel.md)
- [Processes](kern/processes.md)
- [Threads](kern/threads.md)
- [Scheduler](kern/scheduler.md)
- [VirtIO](drivers/virtio.md)
- [VFS](vfs/overview.md)
- [ext4](vfs/ext4.md)
- [JBD2](vfs/jbd2.md)
