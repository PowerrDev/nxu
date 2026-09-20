# Hardware Discovery

`platform_discover()` walks the Device Tree and produces a `platform_t`
describing the machine's resources. It is the only place in NXU where a
device's address is *learned* rather than assumed.

## `platform_t`

```c
typedef struct {
	uint64_t base;
	uint64_t size;
} platform_region_t;

typedef struct {
	platform_region_t memory_regions[PLATFORM_MAX_MEMORY_REGIONS];  /* 8 */
	uint32_t          memory_region_count;

	platform_region_t uart;

	platform_region_t gic_distributor;
	platform_region_t gic_redistributor;

	platform_region_t pcie_ecam;
	uint32_t          pcie_bus_start;
	uint32_t          pcie_bus_end;

	platform_region_t virtio_mmio[PLATFORM_MAX_VIRTIO_MMIO_DEVICES];  /* 32 */
	uint32_t virtio_mmio_intid[PLATFORM_MAX_VIRTIO_MMIO_DEVICES];
	uint32_t virtio_mmio_irq_flags[PLATFORM_MAX_VIRTIO_MMIO_DEVICES];
	uint32_t          virtio_mmio_count;
} platform_t;
```

Roughly 800 bytes, dominated by the VirtIO array. It is filled by value into
caller-provided storage; discovery allocates nothing.

A region with `size == 0` was not found. `platform_discover()` uses exactly this
to decide success, and `platform_dump()` uses it to decide whether to print the
PCI section.

## Interface

```c
bool platform_discover(const dtb_t *dtb, platform_t *platform);
void platform_dump(const platform_t *platform);
```

`platform_discover()` zeroes `*platform`, walks the tree with a discovery
visitor, and then requires that four things were found:

```c
return platform->memory_region_count != 0U
    && platform->uart.size != 0U
    && platform->gic_distributor.size != 0U
    && platform->gic_redistributor.size != 0U;
```

RAM, a console and both GIC frames are mandatory. PCI ECAM and VirtIO are
optional — the kernel boots without them.

**Return:** `true` if the walk succeeded, no visitor error occurred, and all
four mandatory resources were found; `false` otherwise.

## How a node is identified

Two Device Tree properties classify a node:

### `compatible`

A list of NUL-separated strings, most specific first, naming the programming
models the device conforms to. `platform_compatible_contains()` checks whether a
given string appears anywhere in the list — matching one entry is enough, and
matching a *later*, more generic entry is exactly the point of the property.

The strings NXU matches:

| String | Sets |
| --- | --- |
| `arm,pl011` | `is_uart` |
| `arm,gic-v3` | `is_gic` |
| `pci-host-ecam-generic` | `is_pcie` |
| `virtio,mmio` | `is_virtio_mmio` |

The comparison is length-exact within the list: `platform_compatible_contains()`
measures each list item and the target, and requires equal lengths before
comparing bytes, so `arm,pl011` does not match `arm,pl011-extra`.

### `device_type`

A single string. NXU checks only for `memory`, using
`platform_value_string_equals()`, which additionally requires the byte after the
match to be NUL and to lie within the property length.

RAM is also matched by node name: `platform_string_starts_with(name, "memory@")`.
Either condition is sufficient, which makes discovery robust against a tree that
omits one or the other.

## `reg` and address cells

A `reg` property is a list of entries, each consisting of an address followed by
a size. Neither is fixed-width: the widths are set by the **parent** node's
`#address-cells` and `#size-cells` properties, in units of 32-bit cells.

```text
node "/"
    #address-cells = 2      -> children's reg addresses are 2 cells (64-bit)
    #size-cells    = 2      -> children's reg sizes are 2 cells (64-bit)

    node "memory@40000000"
        reg = <0x00000000 0x40000000  0x00000000 0x20000000>
               \_____ address _____/  \______ size ______/
               base = 0x40000000       size = 0x20000000
```

The discovery context tracks this inheritance with a per-depth array:

```c
typedef struct {
	platform_t     *platform;
	platform_node_t nodes[PLATFORM_MAX_DTB_DEPTH];   /* 64 */
	bool            failed;
} platform_discovery_context_t;
```

`platform_begin_node()` initializes `nodes[depth]` and copies the parent's
`child_address_cells`/`child_size_cells` into the child's
`register_address_cells`/`register_size_cells`. The Device Tree defaults —
`#address-cells = 2`, `#size-cells = 1` — are pre-loaded, and overwritten if the
node declares its own.

The parser's depth convention makes this work: properties are reported at their
owning node's depth, so `platform_property()` and `platform_begin_node()` index
the same slot. See [Device tree](device-tree.md).

`platform_read_cells()` assembles a value big-endian, one 32-bit cell at a time,
and rejects a cell count above 2 — nothing NXU understands needs more than 64
bits.

`platform_read_reg(node, entry_index, region)` extracts the *n*th entry, with
overflow-safe bounds checking against `reg_length`.

## Deferred processing

Discovery does its work in `platform_end_node()`, not in `platform_property()`.

This is required because Device Tree properties arrive in file order, and
nothing guarantees that `compatible` precedes `reg`. In the observed QEMU tree
they arrive in the opposite order:

```text
node virtio_mmio@a001200
  property dma-coherent
  property interrupts
  property reg              <-- arrives first
  property compatible       <-- classifies the node
```

Acting on `reg` when it arrives would mean not yet knowing what the node is.
`platform_property()` therefore only *records* — stashing `reg`, `bus-range`,
the cell counts and the classification flags into `nodes[depth]` — and
`platform_end_node()` acts once every property has been seen.

## What each node contributes

| Classification | `platform_end_node()` action |
| --- | --- |
| memory | Append `reg[0]` to `memory_regions[]` if room remains |
| UART | `reg[0]` → `uart` |
| GIC | `reg[0]` → `gic_distributor`, `reg[1]` → `gic_redistributor` |
| PCIe | `reg[0]` → `pcie_ecam`; `bus-range` → `pcie_bus_start`/`_end` |
| VirtIO-MMIO | Append `reg[0]` to `virtio_mmio[]` if room remains |

The classifications are checked in that order and each branch returns, so a node
matching two categories is claimed by the first.

The GIC case is the only one using a second `reg` entry. A GICv3 node's `reg`
lists the distributor frame first and the redistributor region second, which is
exactly what the driver needs.

`bus-range` is two 32-bit big-endian values; discovery requires at least 8 bytes
before reading them.

Most `platform_read_reg()` results are discarded with `(void)`, so a node that
is classified but whose `reg` cannot be parsed leaves the corresponding region
zeroed — which the mandatory-resource check then catches for the UART and GIC.

## Limits and overflow

| Limit | Value | Behavior when exceeded |
| --- | --- | --- |
| `PLATFORM_MAX_MEMORY_REGIONS` | 8 | Additional regions silently ignored |
| `PLATFORM_MAX_VIRTIO_MMIO_DEVICES` | 32 | Additional transports silently ignored |
| `PLATFORM_MAX_DTB_DEPTH` | 64 | `failed = true`; discovery fails |

The first two are silent. On the observed machine there is 1 memory region and
exactly 32 VirtIO transports — precisely at the limit, so a QEMU configuration
with more would lose some without any indication.

Depth overflow is treated as an error rather than ignored, because indexing past
`nodes[]` would corrupt the context. `platform_begin_node()` sets `failed`, and
`platform_discover()` checks it after the walk. Note that the parser's own
`DTB_MAX_DEPTH` is also 64, so the parser would reject such a tree first.

## Failure conditions

| Condition | Result |
| --- | --- |
| Null `dtb` or `platform` | `false` |
| `dtb_walk()` fails (malformed tree) | `false` |
| Depth overflow | `false` |
| No memory region found | `false` |
| No UART found | `false` |
| No GIC distributor frame | `false` |
| No GIC redistributor frame | `false` |
| PCI or VirtIO absent | **not** a failure |

`kern_init()` treats failure as fatal.

## How discovered values flow onward

```text
platform_t
   |
   +--> pmm_init(platform, dtb)
   |       memory_regions[0]          -> the managed RAM range
   |       (regions 1..n are ignored)
   |
   +--> vmm_init(platform)
   |       every memory region        -> Normal, read-write
   |         (the one containing the kernel is split by section)
   |       uart                       -> Device, read-write
   |       gic_distributor            -> Device, read-write
   |       gic_redistributor          -> Device, read-write
   |       pcie_ecam                  -> Device, read-write
   |       virtio_mmio[0..n]          -> Device, read-write
   |
   +--> kern_init()
   |       uart.base                  -> identity-translation check
   |
   +--> platform_dump()               -> boot log
```

Two consumers that *should* be on this list are not:

- **`platform/arm64/uart.c`** hard-codes `0x09000000` and never reads
  `platform->uart.base`. This is partly defensible — the console must work
  before discovery runs — but there is no later switch to the discovered
  address, so a machine with a differently placed PL011 would be mapped
  correctly and then written at the wrong address.
- **`kern/arm64/gic.c`** hard-codes `GICD_BASE` and `GICR_BASE` despite
  running long after discovery. A source `TODO` records the intent to fix this.

See [UART](uart.md) and
[Interrupt controller](../arm64/interrupt-controller.md).

## Concurrency and interrupt context

`platform_discover()` and `platform_dump()` hold no global state and are
reentrant. They must not be called from interrupt context: discovery is not an
interrupt-time operation and `platform_dump()` prints extensively.

`platform_discover()` places a `platform_discovery_context_t` — containing 64
`platform_node_t` entries — on the caller's stack. Combined with the ~800-byte
`platform_t` that `kern_init()` also holds, this is a non-trivial fraction of
the 16 KiB boot stack.

## Current limitations

- Silent truncation at 8 memory regions and 32 VirtIO transports.
- Only `reg[0]` is read for every device except the GIC.
- No `ranges` translation, so an address behind a bus that remaps its children's
  addresses would be recorded untranslated. QEMU `virt` places these devices
  directly on the root bus, so it does not arise today.
- No `status = "disabled"` handling; a disabled node would still be recorded.
- No `interrupts` parsing. The timer PPI is hard-coded as 30 in two places.
- No `chosen` node, so no `stdout-path`, `bootargs` or initrd discovery.
- Nothing is discovered for the CPU: no core count, no `MPIDR` affinities, no
  `enable-method`.
- No `phandle` resolution, so the interrupt-parent relationship is invisible.
- Fixed-size arrays with no growth path.
- No global `platform_t`; the structure lives on `kern_init()`'s stack.

## Source files

- [`platform/arm64/platform.c`](../../platform/arm64/platform.c)
- [`platform/platform.h`](../../platform/platform.h)
- [`platform/dtb.h`](../../platform/dtb.h)

## Related documentation

- [Platform overview](overview.md)
- [Device tree](device-tree.md)
- [UART](uart.md)
- [Physical memory](../vm/physical-memory.md)
- [Virtual memory](../vm/virtual-memory.md)
- [Interrupt controller](../arm64/interrupt-controller.md)
- [Memory map](../memory-map.md)


### VirtIO interrupt specifier

For each `virtio,mmio` node NXU also parses the QEMU `virt` GIC three-cell
`interrupts` property. SPI numbers are converted to architectural INTIDs by
adding 32; PPI numbers are converted by adding 16. Trigger flags are retained
for the GIC driver. Device drivers therefore receive an INTID and never decode
Device Tree interrupt numbering themselves.
