# Platform Layer

The platform layer answers one question for the rest of the kernel: **what
hardware is this, and where is it?**

It is neither architecture code nor core kernel code. AArch64 defines what a
`mrs` instruction does; it says nothing about where a UART lives. The core
kernel decides when to initialize the console; it should not know the console's
address. The platform layer sits between them, parses the firmware-provided
description of the machine, and publishes the result as data.

## Responsibilities

- Validate and parse the flattened Device Tree supplied by firmware.
- Provide a safe, bounds-checked traversal interface over that blob.
- Discover machine resources: RAM regions, the serial console, the interrupt
  controller frames, the PCI ECAM window and bus range, and VirtIO-MMIO
  transports.
- Publish them in a single machine-independent structure, `platform_t`.
- Own the early platform console, so a diagnostic can be printed before anything
  else is trusted.
- Keep machine addresses out of the architecture and core-kernel layers.

## Non-responsibilities

- **Driving devices.** Discovery finds the GIC's frames; it does not program the
  GIC. It finds the VirtIO transports; there is no VirtIO driver.
- **Mapping memory.** `platform_t` describes physical regions.
  [`vmm_init()`](../vm/virtual-memory.md) decides how to map them.
- **Allocating.** Discovery runs before the PMM exists and fills a
  caller-provided structure.
- **Interpreting interrupt routing.** The `interrupts` property is walked past;
  no interrupt number is discovered.
- **Deciding initialization order.** That belongs to `kern_init()`.
- **Enumerating buses.** PCI ECAM is found, but no PCI device is enumerated.

## Structure

```text
firmware (QEMU) builds a flattened Device Tree
     |
     |  x0 = DTB physical address
     v
dtb_init()          validate the header, bounds-check every block
     |
     v
dtb_walk()          token-by-token traversal, calling a visitor
     |
     +--> dtb_dump()            diagnostic visitor: print the tree
     |
     +--> platform_discover()   discovery visitor: fill platform_t
              |
              v
          platform_t
              |
              +--> pmm_init()    RAM base and size
              +--> vmm_init()    every region to map, with its memory type
              +--> platform_dump()
```

`dtb_walk()` is the single traversal primitive, and both consumers are visitors
over it. That keeps the bounds checking, token decoding and alignment handling
in exactly one place.

## Directory layout

```text
platform/
├── dtb.h            Device Tree parser interface
├── platform.h       platform_t and platform_discover()
├── uart.h           Early console interface
└── arm64/
    ├── dtb.c        Parser implementation
    ├── platform.c   Discovery implementation
    └── uart.c       PL011 implementation
```

The interfaces are machine-independent; the implementations live under `arm64/`.
The split anticipates a second machine, though only one exists today. Note that
`dtb.c` is not architecture-specific in any meaningful way — the flattened
Device Tree format is byte-order-defined and portable — so its placement under
`arm64/` reflects the current directory convention rather than a real
dependency.

## Where the boundary currently leaks

Two places bypass the platform layer and hard-code a QEMU `virt` address:

| Location | Address | Discovered equivalent |
| --- | --- | --- |
| [`platform/arm64/uart.c`](../../platform/arm64/uart.c) | `UART_BASE 0x09000000` | `platform->uart.base` |
| [`mach/arm64/gic.c`](../../mach/arm64/gic.c) | `GICD_BASE 0x08000000`, `GICR_BASE 0x080A0000` | `platform->gic_distributor`, `platform->gic_redistributor` |

The UART case is defensible: something must print before the Device Tree has
been parsed, and a bootstrap console has to start somewhere. The GIC case is
not — `gic_init()` runs long after discovery completes, and a source `TODO`
records the intent to fix it. Both are documented in
[Hardware discovery](hardware-discovery.md) and
[Interrupt controller](../arm64/interrupt-controller.md).

## Lifetime and ownership

`platform_discover()` fills a caller-provided `platform_t`. It allocates
nothing, retains no pointer and has no global state. `dtb_init()` likewise fills
a caller-provided `dtb_t` whose fields are pointers **into the blob** — so the
blob must remain valid and mapped for as long as the `dtb_t` is used.

That is why `pmm_init()` permanently reserves the DTB's physical range: the
`dtb_t` in `kern_init()` outlives every subsystem.

Both structures currently live on `kern_init()`'s stack frame, which never
unwinds. There is no global platform accessor, so any future subsystem needing
platform data after initialization must be handed it explicitly.

## Concurrency

The platform layer is stateless and its functions are reentrant with respect to
each other, but they are called only during single-threaded initialization with
IRQs masked. The one exception is the UART, which is used from interrupt
context by the panic path; see [UART](uart.md).

## Current limitations

- One machine supported: QEMU `virt`.
- Discovery runs once, at boot. There is no hot-plug and no re-discovery.
- No global `platform_t`; the structure lives on the boot stack.
- No `interrupts` property parsing, so interrupt numbers are hard-coded.
- No `chosen`, `bootargs`, `stdout-path` or `initrd` handling.
- No memory-reservation-block processing; the field is validated but the
  entries are never read.
- Two hard-coded MMIO bases remain, as above.

## Source files

- [`platform/dtb.h`](../../platform/dtb.h)
- [`platform/platform.h`](../../platform/platform.h)
- [`platform/uart.h`](../../platform/uart.h)
- [`platform/arm64/dtb.c`](../../platform/arm64/dtb.c)
- [`platform/arm64/platform.c`](../../platform/arm64/platform.c)
- [`platform/arm64/uart.c`](../../platform/arm64/uart.c)

## Related documentation

- [Device tree](device-tree.md)
- [Hardware discovery](hardware-discovery.md)
- [UART](uart.md)
- [Architecture](../architecture.md)
- [Initialization](../initialization.md)
- [Memory map](../memory-map.md)


## VirtIO peripherals

The platform layer records each `virtio,mmio` region together with its parsed
GIC INTID and trigger flags. Device identification and feature negotiation are
performed later by the VirtIO core; platform discovery deliberately does not
identify a node as keyboard, mouse, block device or GPU.
