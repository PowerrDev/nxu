# Virtual Memory Manager

The VMM owns the AArch64 stage-1 translation tables and the MMU control
registers. It builds the initial identity map, enables translation, and
thereafter installs, queries, reprotects and removes individual 4 KiB mappings.

For the descriptor and index encodings this document refers to, see
[Translation tables](translation-tables.md).

## Configuration

| Property | Value | Source |
| --- | --- | --- |
| Translation granule | 4 KiB | `TCR_EL1.TG0 = 0`, checked against `ID_AA64MMFR0_EL1.TGran4` |
| Virtual address size | 39 bits (512 GiB) | `VMM_VA_BITS`, `TCR_EL1.T0SZ = 25` |
| Translation levels | L1, L2, L3 | consequence of 39 bits with a 4 KiB granule |
| Physical address size | Discovered | `ID_AA64MMFR0_EL1.PARange` → `TCR_EL1.IPS` |
| Table bases | `TTBR0_EL1` + `TTBR1_EL1` | TTBR0 user / TTBR1 kernel |
| Address spaces | Kernel plus independent user maps | current task owns TTBR0 |

39 bits is the natural choice for a three-level 4 KiB configuration: each level
consumes 9 index bits and the page offset takes 12, giving 9 + 9 + 9 + 12 = 39.
A four-level 48-bit space would need an extra table walk for no current benefit.

### Physical address width discovery

`vmm_init()` reads `ID_AA64MMFR0_EL1` and decodes `PARange` through a table:

| Encoding | Width |
| --- | --- |
| 0 | 32 |
| 1 | 36 |
| 2 | 40 |
| 3 | 42 |
| 4 | 44 |
| 5 | 48 |

An encoding of 6 or above fails initialization. The width is stored in
`g_vmm.physical_bits` and used to validate every physical address before it is
placed in a descriptor; the raw encoding is written to `TCR_EL1.IPS`. The
observed QEMU `cortex-a72` reports encoding 4, giving 44 bits.

### Level coverage

| Level | Index bits | Entries | Each entry covers | Total per table |
| --- | --- | --- | --- | --- |
| L1 | VA[38:30] | 512 | 1 GiB | 512 GiB |
| L2 | VA[29:21] | 512 | 2 MiB | 1 GiB |
| L3 | VA[20:12] | 512 | 4 KiB | 2 MiB |

## Memory types and `MAIR_EL1`

```c
MAIR_EL1 = 0x00000000000004FF
```

| Index | Value | Type | Used for |
| --- | --- | --- | --- |
| 0 | `0xFF` | Normal, inner/outer write-back, read/write-allocate | RAM |
| 1 | `0x04` | Device-nGnRE | UART, GIC, PCI ECAM, VirtIO-MMIO |

The `vmm_memory_type_t` enum maps directly onto these indices. Device mappings
additionally always set both `PXN` and `UXN`, and `vmm_attributes()` rejects
`VMM_PROTECTION_READ_EXECUTE` for Device memory outright — executing from MMIO
is never valid.

Device mappings use outer shareable (`SH = 0b10`); Normal mappings use inner
shareable (`SH = 0b11`).

See [Cache](../arm64/cache.md) for why the distinction matters.

## `TCR_EL1`, `TTBR0_EL1` and `SCTLR_EL1`

`vmm_enable()` writes all four control registers in one inline assembly block:

```asm
dsb ishst              /* all table writes complete and visible */

msr MAIR_EL1,  x0
msr TCR_EL1,   x1
msr TTBR0_EL1, x2
isb                    /* the new configuration takes effect */

tlbi vmalle1           /* discard every stale EL1 translation */
dsb ish
isb

msr SCTLR_EL1, x3      /* set M and WXN: translation is now live */
isb
```

The ordering is the point. The tables must be visible before `TTBR0_EL1` names
them; the TLB must be flushed of anything the boot firmware left behind before
translation is enabled; and the final `isb` ensures the instructions after it
execute under the new regime.

The `SCTLR_EL1` write is read-modify-write and sets two bits:

| Bit | Name | Effect |
| --- | --- | --- |
| 0 | `M` | Stage-1 translation enable |
| 19 | `WXN` | Write permission implies execute-never |

`SCTLR_EL1.C` and `SCTLR_EL1.I` are deliberately left untouched; the caches are
enabled separately by `cache_init()` afterwards.

`WXN` is a hardware backstop for W^X. With it set, any EL1-writable region is
treated as non-executable regardless of what its descriptor says, so a
descriptor bug cannot produce an executable writable page.

## Identity mapping

`vmm_init()` builds the initial address space with the MMU still off. It maps:

| Region | Type | Protection |
| --- | --- | --- |
| RAM below `__text_start` | Normal | read-write |
| `.text` | Normal | read-execute |
| `.rodata` | Normal | read-only |
| `.data`, `.bss` | Normal | read-write |
| RAM above `__kernel_end` | Normal | read-write |
| Other RAM regions | Normal | read-write |
| UART | Device | read-write |
| GIC distributor, redistributor | Device | read-write |
| PCI ECAM | Device | read-write |
| Every VirtIO-MMIO window | Device | read-write |

`vmm_map_range()` walks the range choosing the largest descriptor that fits:

```text
address 1 GiB-aligned and >= 1 GiB remaining  ->  L1 block
address 2 MiB-aligned and >= 2 MiB remaining  ->  L2 block
otherwise                                     ->  L3 page
```

This keeps the identity map small — the observed boot allocates 8 tables in
total — while still permitting page granularity where the kernel's section
permissions require it.

`vmm_set_entry()` tolerates re-installing a descriptor identical to the existing
one. This is required because the 32 VirtIO-MMIO windows are 512 bytes each and
eight share every 4 KiB page, so mapping them one at a time necessarily revisits
pages. Any *differing* descriptor over a valid entry is rejected.

### Kernel section permissions

`vmm_map_kernel_memory()` validates before it maps. It requires that all ten
linker symbols are page-aligned, and that the sections are exactly contiguous:

```c
kernel_start == text_start
text_end     == rodata_start
rodata_end   == data_start
data_end     == bss_start
bss_end      == kernel_end
```

If either check fails, `vmm_init()` fails. This is what guarantees no page
contains bytes from two sections with different permissions — a page that mixed
`.text` and `.data` would have to be both executable and writable, which `WXN`
would then render non-executable, breaking the kernel.

A zero-length section is handled correctly: `vmm_map_span()` returns success
without mapping anything when `start == end`. `.data` is currently empty, so
this path is exercised on every boot.

### Why the identity map matters

Because virtual equals physical everywhere, a physical address returned by the
PMM is immediately usable as a C pointer. That single property is what lets
translation tables be allocated with `pmm_allocate_page()` and used directly,
lets the PMM zero pages by writing through their physical addresses, and lets
the heap's small tier hand out identity-mapped physical pages with no mapping
step at all.

It is also what a higher-half kernel gives up. See
[Roadmap to userland](../roadmap-to-userland.md).

## Public API

### `vmm_init()`

```c
bool vmm_init(const platform_t *platform);
```

Builds the identity address space and enables stage-1 translation. Must be
called exactly once, after `pmm_init()` and after `exception_init()`.

Fails if `platform` is null or reports no memory regions, if the CPU does not
support the 4 KiB granule, if `PARange` is unrecognized, if the root table
cannot be allocated, if any region cannot be mapped, if the kernel section
layout is invalid, or if no memory region contains the kernel.

**Return:** `true` if translation is enabled; `false` otherwise, in which case
the MMU remains off.

### `vmm_map_identity()`

```c
bool vmm_map_identity(uint64_t physical_address, uint64_t size,
                      vmm_memory_type_t memory_type,
                      vmm_protection_t protection);
```

Adds an identity mapping while the MMU is still disabled. It explicitly refuses
to run once `g_vmm.enabled` is set, because it can install block descriptors and
performs no break-before-make or TLB maintenance.

This is an early-boot interface. It is currently **not called by anything** —
`vmm_init()` uses the internal `vmm_map_range()` directly.

**Return:** `true` on success; `false` if the root table does not exist, the MMU
is already enabled, or the range cannot be mapped.

### `vmm_map_page()`

```c
bool vmm_map_page(uint64_t virtual_address, uint64_t physical_address,
                  vmm_memory_type_t memory_type,
                  vmm_protection_t protection);
```

Installs one live 4 KiB L3 mapping. Both addresses must be page-aligned; the
virtual address must be within the 39-bit space and the physical within the
discovered width. Intermediate tables are created on demand from the PMM.

The virtual address must be **currently unmapped**. Mapping over a valid entry
is rejected rather than silently replacing it — permission changes must go
through `vmm_protect_page()`, which does break-before-make properly.

Because the previous entry was invalid, no TLB invalidation is needed: the
architecture does not cache translation faults. `vmm_publish_new_mapping()`
issues `dsb ish; isb` to make the table write visible and synchronize execution.

**Ownership:** the VMM does not take ownership of the physical page. The caller
remains responsible for freeing it.

**Return:** `true` on success; `false` if the MMU is off, either address is
invalid, the attributes are invalid, a table could not be created, a block
descriptor blocks the walk, or the entry is already valid.

### `vmm_unmap_page()`

```c
bool vmm_unmap_page(uint64_t virtual_address, uint64_t *physical_address);
```

Removes one live L3 mapping. Writes the descriptor to zero, then invalidates the
TLB entry with `tlbi vaae1is`. If `physical_address` is non-null it receives the
physical address the mapping referred to.

The order — break, then invalidate — matters: invalidating first would leave a
window in which the walker could re-cache the entry still present in the table.

**Ownership:** the physical page is *returned to the caller*, not freed. The
caller must free it or it leaks.

**Return:** `true` on success; `false` if the MMU is off, the address is
invalid, no L3 entry exists, or the entry is not a valid page descriptor.

### `vmm_protect_page()`

```c
bool vmm_protect_page(uint64_t virtual_address, vmm_protection_t protection);
```

Changes the permissions of an existing L3 mapping, preserving its physical
address and memory type.

Uses **break-before-make**, which is architecturally required when changing the
attributes of a valid descriptor:

```text
1. write 0 to the entry          (break)
2. tlbi vaae1is + dsb + isb      (remove the cached translation)
3. write the new descriptor      (make)
4. dsb ish + isb                 (publish)
```

Writing the new value directly over the old one is not permitted: the hardware
may hold both the old and new translations simultaneously, producing behavior
the architecture leaves unpredictable.

If the new descriptor would be byte-identical to the old one, the function
returns success without touching anything.

**Return:** `true` on success; `false` if the MMU is off, the address is
invalid, no valid L3 page descriptor exists, the existing attribute index is
neither Normal nor Device, or the requested protection is invalid for the memory
type.

### `vmm_query_page()`

```c
bool vmm_query_page(uint64_t virtual_address, vmm_page_mapping_t *mapping);
```

Reads the L3 descriptor and decodes it into:

```c
typedef struct {
	uint64_t          physical_address;
	vmm_memory_type_t memory_type;
	vmm_protection_t  protection;
} vmm_page_mapping_t;
```

This is a *software* walk of the tables, in contrast to `vmm_translate()`, which
asks the hardware. Using both and comparing is how `kern_test_live_vmm()` proves
the tables and the walker agree.

Protection decoding refuses ambiguous descriptors. `AP = 0b00` with `PXN` clear
would be writable and privileged-executable, which NXU never creates
intentionally, so it is reported as an error rather than decoded.

**Return:** `true` on success; `false` if the MMU is off, `mapping` is null, the
address is invalid, no valid L3 page descriptor exists, or the descriptor's
attributes cannot be decoded.

### `vmm_translate()` and `vmm_translate_write()`

```c
bool vmm_translate(uint64_t virtual_address, uint64_t *physical_address);
bool vmm_translate_write(uint64_t virtual_address, uint64_t *physical_address);
```

Perform a hardware address translation using `AT S1E1R` and `AT S1E1W`
respectively, then read the result from `PAR_EL1`:

```asm
at S1E1R, %1     /* or S1E1W */
isb
mrs %0, PAR_EL1
```

The `AT` instructions run a full stage-1 EL1 translation, honouring access
permissions, **without performing the access**. This is what makes them safe for
probing: `vmm_translate_write()` on read-only memory returns `false` instead of
taking a permission fault.

`PAR_EL1` bit 0 is the fault flag. On success the physical address is
reconstructed as `(PAR_EL1 & 0x0000FFFFFFFFF000) | (va & 0xFFF)`, so unlike
`vmm_query_page()` these accept an unaligned address and preserve its offset.

The `isb` between the `AT` and the `PAR_EL1` read is required: `PAR_EL1` is not
a memory location and the read would otherwise be free to execute before the
translation completes.

**Return:** `true` if the translation succeeded; `false` if it faulted for any
reason. The specific fault status in `PAR_EL1` is discarded.

### `vmm_validate_kernel_permissions()`

```c
bool vmm_validate_kernel_permissions(void);
```

Proves, using the hardware, that the kernel's section permissions are what the
descriptors were meant to say:

| Section | Read must | Write must |
| --- | --- | --- |
| `.text` | succeed, and translate to itself | **fail** |
| `.rodata` | succeed, and translate to itself | **fail** |
| `.data` | — | succeed, and translate to itself |
| `.bss` | — | succeed, and translate to itself |

Only the first address of each section is probed, and empty sections pass
trivially.

The "translate to itself" comparison is what makes this a test of the identity
map as well as of the permissions.

**Return:** `true` if every check passes; `false` if the MMU is off or any check
fails.

### `vmm_is_enabled()` and `vmm_dump()`

`vmm_is_enabled()` returns whether `vmm_init()` completed; `vm_kern_init()` uses
it as its precondition. `vmm_dump()` prints whether stage-1 translation is
enabled and the four section ranges; with `-v` it also prints the root table
address, table count, address sizes and the four control registers.

## TLB maintenance

| Operation | Where | Why |
| --- | --- | --- |
| `tlbi vmalle1` | `vmm_enable()` | Discard everything before translation is enabled |
| `tlbi vaae1is` | `vmm_invalidate_page()` | Remove one page's cached translation |
| none | `vmm_map_page()` | Translation faults are not cached, so a first mapping needs no invalidation |

`vaae1is` invalidates by virtual address, all ASIDs, in the inner shareable
domain. For a 4 KiB granule the operand is the virtual **page number**
(`va >> 12`), not the byte address.

Sequenced as `dsb ish; tlbi; dsb ish; isb`: the first `dsb` orders the preceding
table write ahead of the invalidation, the second waits for the invalidation to
complete, and the `isb` synchronizes execution.

`vmm_next_table()` additionally issues `dsb ishst` around installing a new table
descriptor when the MMU is live, so the zeroed child table is visible before its
parent entry becomes valid.

## Concurrency and interrupt context

The VMM takes no locks and is not reentrant.

- `vmm_init()` must be called exactly once.
- `vmm_map_page()`, `vmm_unmap_page()` and `vmm_protect_page()` mutate live
  translation tables and may call `pmm_allocate_page()`. They must **not** be
  called from interrupt context.
- `vmm_query_page()`, `vmm_translate()` and `vmm_translate_write()` do not
  mutate state, but `vmm_translate*()` clobber `PAR_EL1`, which is shared. They
  must not be called from an interrupt that could preempt another translation.
- Break-before-make in `vmm_protect_page()` leaves the page **unmapped** for a
  window. An access from another context during that window would fault.

## Current limitations

- **Block descriptors cannot be split.** `vmm_next_table()` returns `false` when
  it encounters a valid non-table descriptor. Live mapping therefore works only
  where the identity map already used L3 pages, or in ranges it never touched at
  all — which is why the kernel virtual arena sits far outside the identity map.
- **Translation tables are never reclaimed.** Unmapping every page in an L3
  table leaves the table allocated and its parent descriptor valid. There is no
  reference count, so a long-lived kernel fragments table memory permanently.
- **No ASIDs yet.** TTBR1 permanently maps the kernel and TTBR0 can be replaced
  with a task-owned user address space, but address-space switching still uses
  global invalidation rather than ASID-scoped TLB maintenance.
- **Page faults on user memory are resolved, kernel faults are not.** A user
  fault is offered to `vm_fault_user()` (demand-zero, copy-on-write) and
  otherwise becomes a signal; a fault in the kernel itself reaches
  `exception_handle()` and panics. See
  [Demand paging and copy-on-write](demand-paging.md).
- **Only 4 KiB live mappings.** `vmm_map_page()` cannot install a block, so
  large mappings after boot require one call per page.
- **User mappings are explicit.** Task-owned address spaces can install EL0 R-X
  and RW pages; kernel mappings remain inaccessible from EL0.
- **`vmm_map_identity()` is unused.**
- **Failure inside `vmm_init()` after partial mapping leaves allocated tables
  behind**; there is no rollback, though the kernel halts in that case anyway.

## Source files

- [`vm/vmm.c`](../../vm/vmm.c)
- [`vm/vmm.h`](../../vm/vmm.h)
- [`vm/vmm_internal.h`](../../vm/vmm_internal.h)
- [`vm/vmm_tables.c`](../../vm/vmm_tables.c)
- [`vm/vmm_ttbr1.c`](../../vm/vmm_ttbr1.c)
- [`vm/vmm_debug.c`](../../vm/vmm_debug.c)
- [`vm/pmm.h`](../../vm/pmm.h)
- [`makedefs/linker.ld`](../../makedefs/linker.ld)

## Related documentation

- [Translation tables](translation-tables.md)
- [Physical memory](physical-memory.md)
- [Kernel virtual arena](kernel-virtual-arena.md)
- [VM overview](overview.md)
- [System registers](../arm64/system-registers.md)
- [Cache](../arm64/cache.md)
- [Memory map](../memory-map.md)
- [Roadmap to userland](../roadmap-to-userland.md)
