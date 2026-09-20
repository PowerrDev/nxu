# NXU Documentation

NXU is an educational AArch64 kernel. It boots on the QEMU `virt` machine,
runs a permanently higher-half EL1 kernel, discovers hardware from a flattened
Device Tree, manages physical and virtual memory, enters EL0, handles syscalls,
provides `proc`/`task`/`thread` execution objects, performs real AArch64
context switching and EL0 timer preemption, includes VirtIO keyboard, mouse and
block devices, and provides a vnode-based virtual filesystem with ramfs and a
persistent writable ext4 filesystem protected by CRC32C metadata checksums and
JBD2 recovery. SMP is not implemented yet.

This documentation describes **the current implementation**. It is not a design
proposal. Every statement here is derived from the source tree, and behavior
that does not exist yet is either omitted or explicitly marked as a limitation
or as roadmap material. The single exception is
[Roadmap to userland](roadmap-to-userland.md), which is labelled as a roadmap
throughout.

## Source directory boundaries

```text
kern/arm64   Machine-dependent AArch64 code: boot assembly, exception
             vectors and dispatch, GICv3, generic timer, cache control,
             system-register accessors.
kern         Machine-independent core kernel: initialization, process/task/
             thread management, scheduler core and kernel heap.
vfs          Filesystem-independent namespace, vnodes, open files, descriptor
             tables and filesystem implementations.
vm           Memory management: physical page allocator, translation
             tables and MMU control, kernel virtual address arena.
platform     Platform layer: flattened Device Tree parsing, hardware
             discovery, early console.
libk         Freestanding C runtime: fixed-width integer and boolean
             types, minimal string routines.
makedefs     Linker script and build definitions.
```

The rule that these boundaries encode: `kern` and `vm` must not
contain AArch64 instruction sequences beyond what the memory architecture
requires, and neither the core kernel nor the architecture layer may hard-code
machine addresses that the platform layer can discover. The current code
deviates from that rule in two places; both are recorded in
[Hardware discovery](platform/hardware-discovery.md) and
[Interrupt controller](arm64/interrupt-controller.md).

## Kernel-wide documents

- [Architecture](architecture.md) — subsystem boundaries and how the layers fit
  together.
- [Boot](boot.md) — from QEMU's `-kernel` load to the first C instruction.
- [Initialization](initialization.md) — the full phase-by-phase bring-up
  sequence.
- [Memory map](memory-map.md) — physical and virtual address layout.
- [Build system](build-system.md) — toolchain, flags, outputs, QEMU invocation.
- [Roadmap to userland](roadmap-to-userland.md) — planned work, not implemented
  functionality.
- [Root services](root-services.md) — PID 1, logd, patchd and recovery behavior.
- [Testing](testing.md) — `make check`, the test registry, and how to add a test.
- [Service property lists](service-plists.md) — declarative bootd job schema and plist parser limits.

## ARM64 (`kern/arm64`)

- [Overview](arm64/overview.md) — machine-dependent responsibilities.
- [Exceptions](arm64/exceptions.md) — vector table, saved frame, dispatch,
  panic.
- [Interrupt controller](arm64/interrupt-controller.md) — GICv3 distributor,
  redistributor and CPU interface.
- [Timer](arm64/timer.md) — generic physical timer and the 100 Hz tick.
- [Cache](arm64/cache.md) — cache discovery, invalidation and enablement.
- [System registers](arm64/system-registers.md) — reference table of every
  system register NXU touches.

## Core kernel (`kern`)

- [Initialization](kern/initialization.md) — `kern_init` and its
  responsibilities.
- [Kernel console](kern/console.md) — `kprintf`, UART fan-out and retained boot-log replay.
- [Heap](kern/heap.md) — the two-tier kernel allocator.
- [Processes](kern/processes.md) — `proc` identity/hierarchy and `task` execution ownership.
- [Threads](kern/threads.md) — thread lifetime, state, stacks and machine state.
- [Process control](kern/process-control.md) — user-fault containment, signals, `fork` and `exec`.
- [Scheduler](kern/scheduler.md) — processor object, priority run queue and quantum core.

## Virtual filesystem (`vfs`)

- [Overview](vfs/overview.md) — filesystem registry, mount table and namespace.
- [Vnodes](vfs/vnodes.md) — filesystem-independent object ownership and operations.
- [Files and descriptor tables](vfs/file-descriptors.md) — per-open state and per-process descriptors.
- [ramfs](vfs/ramfs.md) — heap-backed validation filesystem.
- [ext4](vfs/ext4.md) — writable allocation, metadata CRC32C, extents, directories and ordered transactions.
- [Btrfs](vfs/btrfs.md) — read-only driver: layered pure core, subvolumes, tests against real Linux-made images.
- [JBD2](vfs/jbd2.md) — write-ahead metadata transactions, commit ordering and crash recovery.

## Virtual memory (`vm`)

- [Overview](vm/overview.md) — the four memory layers and what each one owns.
- [Demand paging and copy-on-write](vm/demand-paging.md) — lazy regions, the fault resolver, `fork` sharing and address-space teardown.
- [Physical memory](vm/physical-memory.md) — the bitmap physical page manager.
- [Virtual memory](vm/virtual-memory.md) — translation tables, MMU control and
  the mapping API.
- [Higher-half kernel](vm/higher-half-kernel.md) — VMA/LMA split, TTBR1 and
  bootstrap-to-runtime transition.
- [Kernel virtual arena](vm/kernel-virtual-arena.md) — `vm_kern` address-range
  allocation.
- [Translation tables](vm/translation-tables.md) — AArch64 descriptor and index
  reference.

## Platform (`platform`)

- [Overview](platform/overview.md) — the platform layer boundary.
- [Device tree](platform/device-tree.md) — the flattened Device Tree parser.
- [Hardware discovery](platform/hardware-discovery.md) — `platform_discover()`
  and `platform_t`.
- [UART](platform/uart.md) — the PL011 early console.

## Runtime (`libk`)

- [Freestanding runtime](libk/runtime.md) — why a kernel supplies its own
  `stdint.h`, `stdbool.h`, memory primitives and CRC32C.

## Implementation status

| Subsystem | Status | Notes |
| --- | --- | --- |
| Boot assembly | Implemented | Single core, QEMU raw-image entry only. |
| Kernel console / PL011 | Implemented | `kprintf` fan-out, 32 KiB history, polled serial primary sink. |
| Device Tree parser | Implemented | Read-only walker, version 17 layout. |
| Platform discovery | Implemented | RAM, UART, GIC, PCI ECAM, VirtIO-MMIO. |
| Exception vectors | Implemented | All 16 entries; IRQ, system calls and user-mode faults are handled; kernel faults panic. |
| GICv3 | Implemented | Single core, PPIs and SPIs, platform-provided MMIO bases. |
| Generic physical timer | Implemented | Periodic, 100 Hz, INTID 30. |
| Physical memory manager | Implemented | Bitmap, first RAM region only. |
| Virtual memory manager | Implemented | TTBR0 user spaces + permanent TTBR1 kernel map, 39-bit VA. |
| Kernel section permissions | Implemented | Validated at boot with `AT S1E1R`/`AT S1E1W`. |
| Cache control | Implemented | Legacy `CCSIDR_EL1` format only. |
| Live page mapping | Implemented | L3 only; block descriptors are never split. |
| Kernel virtual arena | Implemented | 64 MiB, first-fit, 256 allocation records. |
| Kernel heap | Implemented | Two tiers; small ≤ 4016 bytes, large via arena. |
| Higher-half kernel (`TTBR1_EL1`) | Implemented | High-linked ELF with low physical LMA and direct map. |
| User address spaces, EL0, syscalls | Implemented | Bootstrap PID 1, safe user copies, write/get_version/exit. |
| Process/task manager | Implemented | PID 0/1, identity, parent-child tree, zombie lifecycle. |
| Thread subsystem | Implemented | Task-owned threads, TIDs, state bits, priorities, kernel stacks, AArch64 machine state. |
| Scheduler | Implemented | CPU 0 processor object, 64-priority run queue, bootstrap/idle threads, quantum dispatch. |
| AArch64 context switch / preemption | Implemented | Per-thread EL1 stacks, TTBR0 task switching, EL0 timer preemption. |
| VirtIO-MMIO core | Implemented | Modern MMIO transport, split virtqueues, IRQ dispatch. |
| Keyboard | Implemented | VirtIO Input EV_KEY, key state, modifiers, diagnostic US-QWERTY translation. |
| Mouse | Implemented | Relative motion, wheel, buttons, SYN_REPORT packet boundaries. |
| VirtIO Block | Implemented | Synchronous queue-0 I/O through the block-device abstraction. |
| VFS | Implemented | Mount table, vnode operations, pathname lookup, file objects, per-process descriptors. |
| ramfs | Implemented | Heap-backed validation filesystem mounted at `/`. |
| ext4 | Implemented foundation | Writable allocation, CRC32C metadata checksums, depth-zero extent mutation and VFS mount at `/disk`. |
| JBD2 | Implemented foundation | Checksum-v3 metadata transactions, immediate checkpointing and deterministic replay test. |
| Fault containment, signals, `fork`, `exec` | Implemented (arm64) | See [Process control](kern/process-control.md); i386 returns "not supported". |
| Demand paging, copy-on-write | Implemented (arm64) | Anonymous memory only; see [Demand paging](vm/demand-paging.md). |
| SMP | Not implemented | Later-stage work. |

## Boot-time validation

The kernel checks itself during bring-up through `kernel_do_post()`, a staged
power-on self-test (`kern/tests/post.c`), and prints the results. These are
development validation, not a stable interface; their exact output may change
without notice. See [Core kernel initialization](kern/initialization.md).

## Drivers

- [VirtIO core](drivers/virtio.md)
- [Input subsystem](drivers/input.md)
- [Keyboard](drivers/keyboard.md)
- [Mouse](drivers/mouse.md)
- [Interrupt dispatch](kern/interrupts.md)
- [Block storage](drivers/block-storage.md)
