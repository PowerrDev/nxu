# Translation Tables

Reference for the AArch64 stage-1 translation-table format as NXU uses it. The
API that manipulates these tables is documented in
[Virtual memory](virtual-memory.md).

## Address decomposition

NXU uses a 39-bit virtual address space with a 4 KiB granule, which yields
three translation levels:

```text
 38      30 29      21 20      12 11         0
+----------+----------+----------+------------+
| L1 index | L2 index | L3 index | page offset|
+----------+----------+----------+------------+
   9 bits     9 bits     9 bits     12 bits

39-bit VA
    |
    +--> L1 index:     bits [38:30]     512 entries, 1 GiB each
    +--> L2 index:     bits [29:21]     512 entries, 2 MiB each
    +--> L3 index:     bits [20:12]     512 entries, 4 KiB each
    +--> page offset:  bits [11:0]      4096 bytes
```

Bits [63:39] must be zero. `vmm_lower_page_valid()` enforces this by requiring
`virtual_address <= (1 << 39) - 4096`.

Index extraction is a shift and a mask:

```c
#define VMM_INDEX_MASK 0x1FFULL   /* 9 bits */

static uint64_t vmm_index(uint64_t address, uint32_t shift)
{
	return (address >> shift) & VMM_INDEX_MASK;
}
```

with `VMM_L1_SHIFT = 30`, `VMM_L2_SHIFT = 21`, `VMM_L3_SHIFT = 12`.

## Table geometry

| Property | Value | Why |
| --- | --- | --- |
| Index width | 9 bits | 4096-byte table / 8-byte entry = 512 = 2^9 |
| Entries per table | 512 | — |
| Entry size | 8 bytes | 64-bit descriptor |
| Table size | 4096 bytes | Exactly one PMM page |

The choice of 9 index bits is not arbitrary: a table must fit in one translation
granule, and a 4 KiB granule holding 8-byte descriptors holds exactly 512 of
them.

Every table is one PMM page, obtained through `vmm_allocate_table()`, which
relies on `pmm_allocate_page()` returning a **zeroed** page so that every
descriptor in a new table starts invalid.

## Descriptor types

The low two bits classify a descriptor, and their meaning depends on the level:

| Bits [1:0] | L1 | L2 | L3 |
| --- | --- | --- | --- |
| `00` | Invalid | Invalid | Invalid |
| `01` | 1 GiB block | 2 MiB block | Reserved (invalid) |
| `11` | Table | Table | 4 KiB page |

The inversion at L3 is the thing to remember: `0b11` means "table" at L1 and L2
but "page" at L3. NXU encodes this with two constants:

```c
#define VMM_DESC_BLOCK      VMM_DESC_VALID                    /* 0b01 */
#define VMM_DESC_TABLE_PAGE (VMM_DESC_VALID | VMM_DESC_TABLE) /* 0b11 */
```

`vmm_map_l1()` and `vmm_map_l2()` use `VMM_DESC_BLOCK`; `vmm_map_l3()` and
`vmm_make_page_descriptor()` use `VMM_DESC_TABLE_PAGE`.

`vmm_next_table()` distinguishes them when walking:

```c
if ((descriptor & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) {
	/* a block occupies this slot; NXU cannot split it */
	return false;
}
```

## Address mask

```c
#define VMM_DESC_ADDRESS_MASK 0x0000FFFFFFFFF000ULL
```

Bits [47:12] hold the output address, whether that is the next table or the
final page. The low 12 bits are zero because everything is 4 KiB aligned, and
the high 16 bits carry attributes.

The same mask serves three purposes: extracting a next-table pointer, extracting
a mapped physical address, and masking an input address when constructing a
descriptor. It is also used to reassemble the result of `PAR_EL1`.

Block descriptors mask more aggressively, since a block must be aligned to its
own size:

```c
descriptor = (address & ~(VMM_L1_SIZE - 1)) | attributes | VMM_DESC_BLOCK;
```

## Attribute fields

| Field | Bits | NXU values |
| --- | --- | --- |
| `AttrIndx` | [4:2] | 0 = Normal, 1 = Device |
| `NS` | 5 | Not set |
| `AP` | [7:6] | `00` = EL1 rw / EL0 none; `10` = EL1 ro / EL0 none |
| `SH` | [9:8] | `10` outer shareable (Device); `11` inner shareable (Normal) |
| `AF` | 10 | Always set |
| `nG` | 11 | Not set |
| `PXN` | 53 | Set except for read-execute |
| `UXN` | 54 | Always set |

### Attribute index

`AttrIndx` selects one of the eight 8-bit slots in `MAIR_EL1`. NXU programs
two:

| Index | `MAIR_EL1` byte | Meaning |
| --- | --- | --- |
| 0 | `0xFF` | Normal, inner/outer write-back, read/write-allocate |
| 1 | `0x04` | Device-nGnRE |

`vmm_descriptor_memory_type()` decodes an index back to `vmm_memory_type_t` and
rejects any index other than 0 or 1.

### Access permissions

Only two `AP` encodings appear:

| `AP[2:1]` | Encoding | EL1 | EL0 |
| --- | --- | --- | --- |
| `00` | `0ULL << 6` | Read-write | No access |
| `10` | `2ULL << 6` | Read-only | No access |

`AP[1]` is the EL0-access bit and is never set by the kernel VMM, so TTBR1
kernel mappings remain unreachable from EL0. Task-owned TTBR0 mappings use the
separate `vm_address_space_t` permission encodings.

The three `vmm_protection_t` values map onto `AP`, `PXN` and `UXN` as follows:

| `vmm_protection_t` | `AP` | `PXN` | `UXN` | Result |
| --- | --- | --- | --- | --- |
| `VMM_PROTECTION_READ_WRITE` | `00` | 1 | 1 | EL1 read-write, no execute |
| `VMM_PROTECTION_READ_ONLY` | `10` | 1 | 1 | EL1 read-only, no execute |
| `VMM_PROTECTION_READ_EXECUTE` | `10` | 0 | 1 | EL1 read and execute |

Device mappings always set both `PXN` and `UXN`, and `READ_EXECUTE` is rejected
for Device memory.

### `PXN` and `UXN`

- `PXN` — Privileged Execute Never. Instruction fetch at EL1 from this page
  faults.
- `UXN` — Unprivileged Execute Never. Instruction fetch at EL0 faults.

`UXN` is set on every descriptor NXU creates. There is no EL0 code today, and
setting it means no kernel mapping could ever become an EL0 execution target by
accident.

`PXN` is clear only for `VMM_PROTECTION_READ_EXECUTE`, used exclusively for
`.text`. Combined with `SCTLR_EL1.WXN`, which forces execute-never on anything
writable, this yields W^X enforced by hardware.

`vmm_descriptor_protection()` reverses the mapping and deliberately rejects one
combination — `AP == 00` with `PXN` clear, which would be writable and
privileged-executable. NXU never creates it, so encountering it means the
descriptor is corrupt, and reporting an error is more useful than decoding it.

### Access flag

```c
#define VMM_DESC_AF (1ULL << 10U)
```

`AF` is set on every descriptor. With `AF` clear, the first access to a page
raises an Access Flag fault, which an OS can use to track page usage for
reclamation. NXU's fault resolver does not track access flags and there is no
reclamation policy, so leaving `AF` clear would simply produce a fault on first
touch that nothing could fix.

### Shareability

| Type | `SH` | Encoding |
| --- | --- | --- |
| Normal | Inner shareable | `3ULL << 8` |
| Device | Outer shareable | `2ULL << 8` |

Shareability defines the domain across which accesses to the location are
coherent. Inner shareable is the conventional choice for RAM on a single
coherent cluster and is the domain the `tlbi vaae1is` and `dsb ish` operations
target. It has no practical effect on a single core, but choosing it now means
the barriers are already correct for SMP.

## Descriptor construction

```c
*descriptor = (physical_address & VMM_DESC_ADDRESS_MASK)
            | attributes
            | VMM_DESC_TABLE_PAGE;
```

`vmm_make_page_descriptor()` validates the physical address against the
discovered width first, then builds the attributes via `vmm_attributes()`, then
combines. The three parts never overlap: address bits [47:12], attribute bits
[11:2] and [54:53], and type bits [1:0].

## Walking

Two walkers exist, differing only in whether they create missing tables:

| Function | Missing table | Block encountered |
| --- | --- | --- |
| `vmm_next_table()` | Allocates and installs one | Fails |
| `vmm_follow_table()` | Fails | Fails |

`vmm_get_l3_entry(virtual_address, create, &entry)` runs both levels with one or
the other. `create = true` is used by `vmm_map_page()`; `create = false` by
`vmm_unmap_page()`, `vmm_protect_page()` and `vmm_query_page()`.

When `vmm_next_table()` installs a new table with the MMU live, it brackets the
parent-descriptor write with `dsb ishst`, so the zeroed child table is visible
before the descriptor pointing at it becomes valid.

## Why one extra L3 table is needed

The identity map uses the largest descriptor that fits, so RAM would naturally
be covered by 1 GiB L1 blocks. But the kernel's `.text` must be read-execute
while `.rodata` is read-only and `.data`/`.bss` are read-write, and those
boundaries are 4 KiB-aligned, not 1 GiB or 2 MiB.

`vmm_map_range()` therefore falls back level by level:

```text
map RAM base .. __text_start        aligned 1 GiB? -> L1 block
                                    else 2 MiB?    -> L2 block
                                    else           -> L3 pages
map __text_start .. __text_end      4 KiB granularity required -> L3 pages
map __rodata_start .. __rodata_end  4 KiB granularity required -> L3 pages
...
```

Covering the kernel image at page granularity requires an L2 table for the
containing 1 GiB region and at least one L3 table for the containing 2 MiB
region — the kernel is about 72 KiB, so one L3 table (covering 2 MiB) suffices.
That table is what makes per-section permissions expressible at all.

The observed boot allocates 8 tables in total: the L1 root, plus the L2/L3
tables needed for the kernel image and for the several MMIO regions that are not
1 GiB or 2 MiB aligned.

## Why live mappings leave empty tables allocated

`vmm_unmap_page()` clears the L3 entry and invalidates the TLB. It does not
examine the other 511 entries in that table, and it does not clear the L2
descriptor pointing at it.

Reclaiming a table would require knowing that every one of its entries is
invalid — either by scanning 512 entries on every unmap, or by maintaining a
per-table occupancy count, neither of which exists. There is also no back
pointer from a table to its parent descriptor, so the L2 entry could not be
cleared without re-walking from the root.

The consequence: an L3 table allocated to satisfy an arena allocation stays
allocated for the life of the kernel even after every page in it is freed. Each
such table permanently costs one 4 KiB physical page. Over the arena's 64 MiB
that is at most 32 L3 tables plus their L2 parents — bounded, but never
returned.

## Constant reference

| Constant | Value | Meaning |
| --- | --- | --- |
| `VMM_VA_BITS` | 39 | Virtual address width |
| `VMM_INDEX_MASK` | `0x1FF` | 9-bit index mask |
| `VMM_L1_SHIFT` / `VMM_L2_SHIFT` / `VMM_L3_SHIFT` | 30 / 21 / 12 | Index positions |
| `VMM_L1_SIZE` / `VMM_L2_SIZE` / `VMM_L3_SIZE` | 1 GiB / 2 MiB / 4 KiB | Coverage per entry |
| `VMM_DESC_VALID` | `1 << 0` | Valid bit |
| `VMM_DESC_TABLE` | `1 << 1` | Table/page bit |
| `VMM_DESC_TYPE_MASK` | `0x3` | Type field |
| `VMM_DESC_BLOCK` | `0b01` | Block descriptor |
| `VMM_DESC_TABLE_PAGE` | `0b11` | Table (L1/L2) or page (L3) |
| `VMM_DESC_ADDRESS_MASK` | `0x0000FFFFFFFFF000` | Output address, bits [47:12] |
| `VMM_DESC_ATTR(i)` | `i << 2` | `AttrIndx` |
| `VMM_DESC_ATTR_MASK` | `7 << 2` | `AttrIndx` field |
| `VMM_DESC_AP_MASK` | `3 << 6` | `AP` field |
| `VMM_DESC_AP_READ_ONLY` | `2 << 6` | EL1 read-only |
| `VMM_DESC_SH_OUTER` | `2 << 8` | Outer shareable |
| `VMM_DESC_SH_INNER` | `3 << 8` | Inner shareable |
| `VMM_DESC_AF` | `1 << 10` | Access flag |
| `VMM_DESC_PXN` | `1 << 53` | Privileged execute never |
| `VMM_DESC_UXN` | `1 << 54` | Unprivileged execute never |

## Current limitations

- **Three levels only.** A 48-bit address space would need a fourth.
- **Blocks cannot be split.** A valid block descriptor blocks any finer-grained
  mapping in its range.
- **Pages cannot be coalesced** back into blocks.
- **Tables are never reclaimed.**
- **`nG` is never set** and there is no ASID handling, so all mappings are
  global.
- **`NS` is never set.** NXU runs in a single security state.
- **Contiguous hint bit (52) is unused**, so the TLB cannot coalesce adjacent
  entries.
- **No hardware access-flag or dirty-bit management** is enabled.

## Source files

- [`vm/vmm.c`](../../vm/vmm.c)
- [`vm/vmm.h`](../../vm/vmm.h)
- [`vm/vmm_internal.h`](../../vm/vmm_internal.h)
- [`vm/vmm_tables.c`](../../vm/vmm_tables.c)
- [`vm/vmm_ttbr1.c`](../../vm/vmm_ttbr1.c)
- [`vm/vmm_debug.c`](../../vm/vmm_debug.c)

## Related documentation

- [Virtual memory](virtual-memory.md)
- [VM overview](overview.md)
- [Physical memory](physical-memory.md)
- [Kernel virtual arena](kernel-virtual-arena.md)
- [System registers](../arm64/system-registers.md)
- [Cache](../arm64/cache.md)
- [Memory map](../memory-map.md)
