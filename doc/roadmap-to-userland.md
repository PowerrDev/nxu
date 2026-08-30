# Roadmap to Userland

> **This document is a roadmap, not documentation of existing functionality.**
>
> Everything in the "Remaining work" sections below is **not implemented**. No
> code in the repository provides it. Nothing here should be cited as a
> description of how NXU behaves. The rest of `doc/` describes the kernel as
> it actually exists.

## Completed

The following are implemented and documented elsewhere in `doc/`:

| Capability | Documentation |
| --- | --- |
| Boot and early console | [Boot](boot.md), [UART](platform/uart.md) |
| Device Tree parsing and platform discovery | [Device tree](platform/device-tree.md), [Hardware discovery](platform/hardware-discovery.md) |
| Exception vectors, frame and dispatch | [Exceptions](arm64/exceptions.md) |
| GICv3 distributor, redistributor, CPU interface | [Interrupt controller](arm64/interrupt-controller.md) |
| Generic physical timer at 100 Hz | [Timer](arm64/timer.md) |
| Physical memory manager | [Physical memory](vm/physical-memory.md) |
| Virtual memory manager, identity map, MMU enable | [Virtual memory](vm/virtual-memory.md) |
| Per-section kernel permissions, hardware-validated | [Virtual memory](vm/virtual-memory.md) |
| Instruction and data caches | [Cache](arm64/cache.md) |
| Live page mapping, query, protect, unmap | [Virtual memory](vm/virtual-memory.md) |
| Kernel virtual arena | [Kernel virtual arena](vm/kernel-virtual-arena.md) |
| Two-tier multi-page kernel heap | [Heap](kern/heap.md) |

## Remaining work

In dependency order:

```text
1. TTBR1 higher-half kernel
        |
        v
2. Per-process TTBR0 address spaces
        |
        v
3. EL0 exception handling
        |
        v
4. SVC syscall path
        |
        +----> 5. Initramfs
        |             |
        |             v
        |      6. ELF64 loader
        |             |
        v             v
       7. First user process
```

Steps 1-4 can be validated with a hard-coded EL0 test before any loader exists.
Steps 5-7 are what turn that test into a real process.

---

## 1. `TTBR1` higher-half kernel

**Why the kernel must move.** AArch64 gives EL1 two translation table bases:
`TTBR0_EL1` for the low half of the virtual address space and `TTBR1_EL1` for
the high half, selected by the top bits of the address. The split exists to
solve one problem: a user process needs a private address space, and the kernel
needs mappings that survive a switch to it.

If the kernel lived in `TTBR0_EL1` alongside the process, every address-space
switch would unmap the kernel — including the code performing the switch. Moving
kernel mappings into `TTBR1_EL1` means switching processes is a single write to
`TTBR0_EL1`, with every kernel mapping untouched.

**Current state.** Implemented. NXU is linked at a permanent higher-half VMA,
boots the same bytes through their physical LMA, builds TTBR1, transitions PC/SP/
VBAR_EL1 to the higher half, installs a RAM/MMIO direct map and then releases
TTBR0 for userspace.

**What the change involves:**

- Clear `TCR_EL1.EPD1` and configure `T1SZ` for the kernel's virtual address
  size.
- Build a second root table and install it in `TTBR1_EL1`.
- Relink the kernel at a high address, or introduce a physical-to-virtual offset
  applied to every kernel pointer derived from a physical address.
- Adopt an ASID scheme so `TLBI` operations can be scoped to one address space
  rather than flushing everything.

**What it costs.** The identity map is what makes NXU's memory management
simple: a physical address from the PMM is directly usable as a pointer. Three
places depend on that and would all need rework:

| Dependency | Current behavior |
| --- | --- |
| `vmm_allocate_table()` | Uses the physical table address as a `uint64_t *` |
| `pmm_allocate_page()` | Zeroes a page by writing through its physical address |
| Heap small tier | Hands out identity-mapped physical pages as allocations |

Each needs a physical-to-virtual mapping function, typically a fixed offset
applied to a permanent direct map of all RAM placed in the `TTBR1` half.

## 2. Per-process `TTBR0` address spaces

**Why `TTBR0` represents the current process.** Once the kernel is in
`TTBR1_EL1`, `TTBR0_EL1` describes nothing the kernel needs. It becomes the
natural home for whichever process is currently running: switching processes is
a write to `TTBR0_EL1` plus the necessary synchronization, and the kernel's own
mappings — including the code executing the switch and the stack it is running
on — remain valid throughout.

**What it involves:**

- An address-space object owning a root table, a set of mappings and an ASID.
- Creation and destruction, with page-table teardown — which needs the
  translation-table reclamation the VMM currently lacks.
- Mappings with `AP[1]` set, so EL0 can actually access them. Today every NXU
  descriptor uses `AP[1] = 0` and sets `UXN`, so nothing is EL0-reachable.
- Per-process `nG` descriptors and ASID assignment, so a switch does not require
  a full TLB flush.
- A per-thread kernel stack for handling exceptions from that process.

## 3. EL0 exception handling

**Current state.** Implemented for the bootstrap process. NXU enters AArch64
EL0, classifies lower-EL vectors, accepts SVC64, safely copies user memory and
returns syscall results through the saved exception frame.

**What it involves:**

- Entering EL0 at all: set `SPSR_EL1` to describe EL0t, set `ELR_EL1` to the
  user entry point, set `SP_EL0` to the user stack, and `eret`.
- Distinguishing exceptions from EL0 from those at EL1. The vector number
  already encodes this — entries 8-11 versus 4-7 — so the frame carries the
  information.
- Making instruction and data aborts from EL0 **recoverable**: a faulting user
  process must be terminated, not panic the kernel.
- Writing `ELR_EL1` and `SPSR_EL1` back from the frame on return. The current
  exit path does not, because NXU never modifies the return context; a handler
  that redirects a thread requires it.
- A per-thread kernel stack, so a user thread's exception does not run on the
  single boot stack.

**Minimal hard-coded EL0 test.** Before any loader exists, the whole path can be
proven with:

1. A few AArch64 instructions embedded in the kernel image — for example an
   infinite loop, or an `SVC #0`.
2. One physical page from the PMM, mapped into a `TTBR0` address space at a
   fixed user virtual address with `AP[1]` set and `UXN` clear, holding a copy of
   those instructions.
3. A second page mapped read-write for a user stack.
4. `SPSR_EL1` set for EL0t, `ELR_EL1` set to the user entry, `SP_EL0` set to the
   user stack top, then `eret`.
5. Confirmation that vector entry 8 or 9 fires and reports EL0 as the origin.

That test needs steps 1-3 but neither an initramfs nor an ELF loader.

## 4. SVC syscall path

**What it involves:**

- Recognizing EC `0x15` (Supervisor Call) at vector entry 8 rather than
  panicking with trap type 4 as today.
- A calling convention: the syscall number and arguments in `x0`-`x7`, the
  result returned in `x0` through the saved frame.
- A dispatch table, replacing the current hard-coded INTID comparison pattern.
- Argument validation — every pointer from EL0 must be checked to lie within
  the calling process's address space before the kernel dereferences it. This is
  the single most important correctness boundary in the whole design.
- Safe copy-in and copy-out helpers that can fail rather than fault.
- Enough syscalls to be useful: write, exit, and eventually the rest.

The ISS field of `ESR_EL1` carries the `SVC` immediate, so a syscall number can
be encoded there instead of in a register if desired.

## 5. Initramfs

**What it involves:**

- Reading the `chosen` node's `linux,initrd-start` and `linux,initrd-end`
  properties. Discovery does not currently parse the `chosen` node at all.
- Reserving the initramfs range in the PMM, exactly as the DTB range is
  reserved today.
- Parsing a simple archive format — `cpio` in the `newc` variant is the usual
  choice, being trivially parseable and requiring no compression.
- A lookup interface to find a named file within the archive.

No block device, filesystem or driver is needed: the archive is already in RAM.

## 6. ELF64 loader

**What it involves:**

- Validating the ELF header: magic, class `ELFCLASS64`, data
  `ELFDATA2LSB`, machine `EM_AARCH64`, type `ET_EXEC` (or `ET_DYN` for a
  position-independent executable).
- Walking the program headers and mapping each `PT_LOAD` segment into the
  process address space with permissions derived from its `p_flags`.
- Handling `p_memsz > p_filesz` by zeroing the difference — the `.bss`
  equivalent for a user program.
- Setting up a user stack, and eventually an initial stack frame carrying
  `argc`, `argv` and an auxiliary vector.
- **Instruction cache synchronization.** Code written through the data cache is
  not visible to instruction fetch. Each loaded executable range needs
  `DC CVAU` / `DSB ISH` / `IC IVAU` / `DSB ISH` / `ISB`, stepping by the line
  sizes `CTR_EL0` reports. NXU has neither operation today; see
  [Cache](arm64/cache.md).

## 7. First user process

Bringing the pieces together: read a binary from the initramfs, create an
address space, load it, build a user stack, and `eret` to its entry point.

NXU has now crossed this milestone with one bootstrap EL0 process. A thread
model and scheduler foundation also exist, although actual AArch64 scheduler
dispatch still uses the bootstrap bridge.

---

## What can wait

The following are conventionally treated as fundamental but are **not**
prerequisites for a first user process. Deferring them keeps the path above as
short as possible:

| Feature | Why it can wait |
| --- | --- |
| **A full scheduler** | The processor/run-queue foundation now exists. AArch64 context switching and timer-driven preemption remain additive next stages. |
| **SMP** | Every current subsystem assumes one core. Adding cores means adding locking everywhere, which is far easier once the interfaces have stabilized around a working userland. |
| **Networking** | No user process needs it to exist. |
| **PCI drivers** | The ECAM window is already discovered; enumeration and drivers add nothing to the userland path. |
| **A disk filesystem** | The initramfs is already in RAM. A block driver plus a filesystem is a large body of work that the first process does not need. |
| **VirtIO drivers** | The transports are discovered but nothing needs them yet. |
| **Signals and IPC** | These become useful once multiple independently scheduled workloads exist. The thread object itself is now implemented. |
| **Demand paging, copy-on-write, swap** | Optimizations over a working eager loader. |

The ordering principle: prefer the shortest path to a running user process, then
build outward. Each of the above becomes substantially easier to design once
there is a real EL0 workload to design against.

## Prerequisites within the current code

Several existing limitations become blocking as this work proceeds:

| Limitation | Blocks | Documented in |
| --- | --- | --- |
| Translation tables are never reclaimed | Address-space destruction | [Translation tables](vm/translation-tables.md) |
| Block descriptors cannot be split | Fine-grained mapping over identity-mapped ranges | [Virtual memory](vm/virtual-memory.md) |
| No locking anywhere | SMP, and any allocation from interrupt context | [VM overview](vm/overview.md) |
| No `panic()` helper; failures halt inline | Recoverable user faults | [Core kernel initialization](kern/initialization.md) |
| No physical page reference counting | Shared mappings, copy-on-write | [Physical memory](vm/physical-memory.md) |
| No `DC CVAU` / `IC IVAU` | Loading executable code | [Cache](arm64/cache.md) |
| GIC MMIO bases hard-coded | Any non-QEMU machine | [Interrupt controller](arm64/interrupt-controller.md) |
| `platform_t` lives on the boot stack | Any subsystem needing platform data later | [Hardware discovery](platform/hardware-discovery.md) |
| No `memcpy` in `libk` | The ELF loader | [Freestanding runtime](libk/runtime.md) |

## Related documentation

- [Architecture](architecture.md)
- [Virtual memory](vm/virtual-memory.md)
- [Translation tables](vm/translation-tables.md)
- [Exceptions](arm64/exceptions.md)
- [Cache](arm64/cache.md)
- [System registers](arm64/system-registers.md)
- [Hardware discovery](platform/hardware-discovery.md)
- [Freestanding runtime](libk/runtime.md)


## Completed input foundation

The QEMU target now has a reusable modern VirtIO-MMIO transport, split
virtqueues, DTB-derived VirtIO interrupts, VirtIO Input, keyboard state and
relative mouse state. The next storage milestone is a block-device abstraction
and VirtIO Block, followed by VFS and ext4.
