# Memory Map

This document describes the physical and virtual address layout of a running
NXU kernel on the QEMU `virt` machine.

NXU uses two translation regimes. TTBR0 is the bootstrap/user low-half
address space. TTBR1 permanently maps the kernel and a direct map of physical
RAM/MMIO at `VMM_HIGHER_HALF_BASE | physical_address`. The kernel ELF therefore
has distinct virtual (VMA) and physical load (LMA) addresses.

## Address categories

NXU addresses fall into five categories, and it matters which one an address
belongs to:

| Category | Where it comes from | Example |
| --- | --- | --- |
| Compile-time fixed | Linker script or a `#define` | Kernel load address, arena base |
| DTB-discovered | `platform_discover()` | RAM base/size, UART, GIC, PCI, VirtIO |
| Firmware-placed | Supplied in `x0` at boot | The DTB itself |
| Dynamically allocated | `pmm_allocate_page()` | Translation tables, heap arenas |
| Reserved virtual | `vm_kern` constants | `0x1000000000`-`0x1004000000` |

Only two addresses in the entire kernel are hard-coded MMIO: the PL011 base in
`platform/arm64/uart.c` and the GIC distributor/redistributor bases in
`mach/arm64/gic.c`. Both duplicate values that discovery already
recovers. See [Hardware discovery](platform/hardware-discovery.md).

## Compile-time fixed addresses

| Symbol / constant | Value | Source |
| --- | --- | --- |
| Kernel physical load address (LMA) | `0x40080000` | [`makedefs/linker.ld`](../makedefs/linker.ld) |
| Kernel virtual link address (VMA) | `0xFFFFFF8040080000` | [`makedefs/linker.ld`](../makedefs/linker.ld) |
| Kernel virtual offset | `0xFFFFFF8000000000` | [`makedefs/linker.ld`](../makedefs/linker.ld) |
| Vector table alignment | 2048 bytes | [`makedefs/linker.ld`](../makedefs/linker.ld) |
| Section alignment | 4096 bytes | [`makedefs/linker.ld`](../makedefs/linker.ld) |
| Boot stack size | 16384 bytes | [`mach/arm64/start.S`](../mach/arm64/start.S) |
| `PMM_PAGE_SIZE` | `4096` | [`vm/pmm.h`](../vm/pmm.h) |
| `VM_KERN_BASE` | `0x0000001000000000` | [`vm/vm_kern.h`](../vm/vm_kern.h) |
| `VM_KERN_SIZE` | `0x0000000004000000` (64 MiB) | [`vm/vm_kern.h`](../vm/vm_kern.h) |
| `VM_KERN_END` | `0x0000001004000000` | [`vm/vm_kern.h`](../vm/vm_kern.h) |
| Hard-coded UART base | `0x09000000` | [`platform/arm64/uart.c`](../platform/arm64/uart.c) |
| Hard-coded `GICD_BASE` | `0x08000000` | [`mach/arm64/gic.c`](../mach/arm64/gic.c) |
| Hard-coded `GICR_BASE` | `0x080A0000` | [`mach/arm64/gic.c`](../mach/arm64/gic.c) |

## DTB-discovered regions

These are read from the Device Tree at every boot; NXU does not assume them.
The values shown are what QEMU `virt` with `-m 512M` reports, as observed in the
boot log, and are included only to make the layout concrete.

| Region | `platform_t` field | Observed base | Observed size | Mapped as |
| --- | --- | --- | --- | --- |
| RAM | `memory_regions[0]` | `0x40000000` | 512 MiB | Normal |
| PL011 UART | `uart` | `0x09000000` | 4 KiB | Device |
| GIC distributor | `gic_distributor` | `0x08000000` | 64 KiB | Device |
| GIC redistributor | `gic_redistributor` | `0x080A0000` | 15.375 MiB | Device |
| PCI ECAM | `pcie_ecam` | `0x4010000000` | 256 MiB | Device |
| PCI bus range | `pcie_bus_start`/`_end` | 0 | 255 | n/a |
| VirtIO-MMIO ×32 | `virtio_mmio[]` | `0x0A000000` + n·`0x200` | 512 B each | Device |

The 32 VirtIO windows are 512 bytes each and consecutive, so eight of them share
each 4 KiB page. `vmm_set_entry()` explicitly tolerates re-installing an
identical descriptor for exactly this reason.

The DTB itself is placed by QEMU and its address arrives in `x0`. On the
observed machine it lands at `0x48000000` with a total size of 1 MiB, but
nothing in NXU depends on that; the PMM reserves whatever range
`dtb->base`/`dtb->total_size` describe.

## Physical layout

```text
0x0000000008000000  +-----------------------------+
                    | GIC distributor    (Device) |  64 KiB
0x0000000008010000  +-----------------------------+
                    :            ...              :
0x00000000080A0000  +-----------------------------+
                    | GIC redistributor  (Device) |
0x0000000008F80000  +-----------------------------+
                    :            ...              :
0x0000000009000000  +-----------------------------+
                    | PL011 UART         (Device) |  4 KiB
0x0000000009001000  +-----------------------------+
                    :            ...              :
0x000000000A000000  +-----------------------------+
                    | VirtIO-MMIO x32    (Device) |  512 B each
0x000000000A004000  +-----------------------------+
                    :            ...              :
0x0000000040000000  +=============================+  RAM base
                    | QEMU boot stub / free RAM   |  reserved by PMM
0x0000000040080000  +-----------------------------+  __kernel_start
                    | .text                 r-x   |
                    |   .text.boot (_start)       |
                    |   .text.vectors (2048-al.)  |
0x0000000040087000  +-----------------------------+  __text_end
                    | .rodata               r--   |
0x0000000040089000  +-----------------------------+  __rodata_end
                    | .data                 rw-   |  (currently empty)
0x0000000040089000  +-----------------------------+  __data_end
                    | .bss                  rw-   |
                    |   includes the 16 KiB stack |
0x0000000040092000  +-----------------------------+  __kernel_end
                    | PMM bitmap            rw-   |  page-aligned, reserved
0x0000000040096000  +-----------------------------+
                    | translation tables          |  PMM-allocated, 4 KiB each
                    | heap small arena pages      |  PMM-allocated
                    | vm_kern backing pages       |  PMM-allocated
                    |                             |
                    | free RAM                    |
0x0000000048000000  +-----------------------------+  DTB (firmware-placed)
                    | Device Tree Blob            |  reserved by PMM
0x0000000048100000  +-----------------------------+
                    | free RAM                    |
0x0000000060000000  +=============================+  RAM end
                    :            ...              :
0x0000004010000000  +-----------------------------+
                    | PCI ECAM           (Device) |  256 MiB
0x0000004020000000  +-----------------------------+
```

The section boundaries shown are from an observed build. `.data` is currently
empty, so `__data_start == __data_end`; the VMM handles a zero-length span by
mapping nothing.

## Virtual layout

NXU uses both translation regimes. `TTBR0_EL1` owns the active low-half user
address space, while `TTBR1_EL1` permanently owns the higher-half kernel and
direct map. The kernel ELF is linked at its TTBR1 VMA but loaded at a low
physical LMA during bootstrap.

```text
0x0000000000000000  +-----------------------------+
                    | identity map of all Device  |
                    | MMIO and all RAM            |  == physical layout above
0x0000004020000000  +-----------------------------+
                    | unmapped (translation fault)|
0x0000001000000000  +-----------------------------+  VM_KERN_BASE
                    | kernel virtual arena        |  64 MiB reserved
                    |   16384 page slots          |
                    |   backed on demand by       |
                    |   scattered physical pages  |
0x0000001004000000  +-----------------------------+  VM_KERN_END
                    | unmapped (translation fault)|
0x0000008000000000  +-----------------------------+  39-bit VA limit
                    | outside TTBR0_EL1 range     |
0xFFFFFFFFFFFFFFFF  +-----------------------------+
```

Note that `VM_KERN_BASE` (`0x1000000000`, 64 GiB) is numerically *below* the
PCI ECAM window (`0x4010000000`, 256 GiB), so the two blocks in the diagram are
not in ascending order relative to the identity map. They do not overlap: the
arena occupies `[0x1000000000, 0x1004000000)` and ECAM occupies
`[0x4010000000, 0x4020000000)`.

The arena base was chosen to sit far above any address the identity map can
produce on this machine, so that an arena pointer is immediately recognizable
and `vm_kern_contains()` is a simple range test.

## Reserved versus allocatable physical pages

The PMM distinguishes two kinds of used page:

- **Permanently reserved** — marked used during `pmm_init()` and never freed:
  everything from the RAM base through `__kernel_end`, the PMM bitmap storage,
  and the DTB. There is no API to release them.
- **Allocated** — handed out by `pmm_allocate_page()` and returnable via
  `pmm_free_page()`: translation tables, heap small arenas, and `vm_kern`
  backing pages.

Reserving from the RAM base rather than from `__kernel_start` deliberately
sacrifices the 512 KiB below the kernel. That region holds QEMU's boot stub and
is not worth reclaiming.

On the observed 512 MiB machine this yields 131072 total pages, of which 406 are
reserved at the end of `pmm_init()`: 146 for the RAM base through
`__kernel_end`, 4 for the bitmap, and 256 for the 1 MiB DTB.

## Translation table footprint

The identity map is built with the largest descriptor that fits, so it is cheap:
the observed boot allocates 8 translation tables (32 KiB) in total, one L1 root
plus L2 and L3 tables only where sub-1 GiB or sub-2 MiB granularity is needed —
principally to give the kernel's `.text` and `.rodata` page-level permissions.

Tables allocated for the arena are never reclaimed, even when every page in
them is unmapped. See [Translation tables](vm/translation-tables.md).

## Source files

- [`makedefs/linker.ld`](../makedefs/linker.ld)
- [`vm/pmm.c`](../vm/pmm.c)
- [`vm/vmm.c`](../vm/vmm.c)
- [`vm/vm_kern.h`](../vm/vm_kern.h)
- [`platform/arm64/platform.c`](../platform/arm64/platform.c)

## Related documentation

- [Architecture](architecture.md)
- [Boot](boot.md)
- [Physical memory](vm/physical-memory.md)
- [Virtual memory](vm/virtual-memory.md)
- [Kernel virtual arena](vm/kernel-virtual-arena.md)
- [Translation tables](vm/translation-tables.md)
- [Hardware discovery](platform/hardware-discovery.md)

## Current limitations

- Only `memory_regions[0]` is managed by the PMM. A machine with several RAM
  banks would have the rest identity-mapped by the VMM but never allocatable.
- The identity map means the virtual layout carries no information the physical
  layout does not, apart from the arena.
- The arena base and size are compile-time constants with no way to grow.
- No guard pages anywhere: not below the boot stack, not around arena
  allocations.
