# Physical Memory Manager

The PMM owns every 4 KiB physical page in the managed RAM region. It is the
bottom of the memory stack: it knows nothing about virtual addresses,
permissions or mappings.

## Page size

```c
#define PMM_PAGE_SIZE 4096ULL
```

4 KiB matches the AArch64 translation granule NXU selected (`TCR_EL1.TG0 = 0`)
and the L3 descriptor size, so one PMM page is exactly one virtual page. The
constant is exported from `pmm.h` and is used by the VMM, `vm_kern` and the heap
rather than each defining its own.

## Bitmap organization

One bit per page:

| Bit value | Meaning |
| --- | --- |
| 0 | Free — the page may be allocated |
| 1 | Used — allocated, or permanently reserved |

The bitmap makes no distinction between "allocated" and "reserved". A reserved
page is simply one that was set during `pmm_init()` and that nothing will ever
free, since there is no API that could.

`pmm_init()` memsets the bitmap to zero — everything free — and then reserves
what must not be handed out. Starting from "all free" and subtracting is
simpler and less error-prone than starting from "all used" and adding.

## Address arithmetic

Given a physical address inside the managed range:

```text
page index    = (address - memory_base) / PMM_PAGE_SIZE
bitmap byte   = page_index >> 3            /* page_index / 8  */
bitmap bit    = page_index & 7             /* page_index % 8  */
bit mask      = 1 << bitmap_bit
```

And in the other direction:

```text
physical address = memory_base + page_index * PMM_PAGE_SIZE
```

`pmm_address_to_index()` performs the forward conversion and rejects any address
outside `[memory_base, memory_end)`, so it doubles as the range check.

The shift and mask forms are used directly in `pmm_bitmap_is_used()`,
`pmm_bitmap_set_used()` and `pmm_bitmap_set_free()`.

## RAM region discovery

`pmm_init()` takes `platform->memory_regions[0]` and aligns it *inward*:

```c
memory_base = align_up(region->base, PMM_PAGE_SIZE);
memory_end  = align_down(region->base + region->size, PMM_PAGE_SIZE);
```

Aligning inward guarantees every page in `[memory_base, memory_end)` is entirely
inside real RAM. A partial page at either end is discarded rather than
half-managed.

`memory_end <= memory_base` fails initialization.

## Bitmap placement

The bitmap must live somewhere, and it must live somewhere that is not itself
managed dynamically — the PMM cannot allocate its own bitmap.

`pmm_find_bitmap_location()` places it at the first page-aligned address above
`__kernel_end`. If that would overlap the DTB, it moves to the first
page-aligned address above the end of the DTB instead. It then checks the whole
storage range lies within `[memory_base, memory_end)`.

Two sizes are tracked:

| Field | Meaning |
| --- | --- |
| `bitmap_bytes` | `(page_count + 7) / 8` — the logical size |
| `bitmap_storage_bytes` | `bitmap_bytes` rounded up to a page — the reserved size |

The rounded-up size is what gets reserved and zeroed, so the bitmap always
occupies whole pages and no other allocation can land in its final partial page.

On the observed 512 MiB machine this is 131072 pages, a 16384-byte bitmap
occupying exactly four pages at `0x40092000`.

## Reservations

Three ranges are reserved permanently, in this order:

| Range | From | To | Why |
| --- | --- | --- | --- |
| Boot area and kernel image | `memory_base` | `__kernel_end` | The running kernel, its stack and its globals |
| PMM bitmap | `bitmap_address` | `+ bitmap_storage_bytes` | The allocator's own metadata |
| Device Tree Blob | `dtb->base` | `+ dtb->total_size` | Still referenced by `dtb_t` |

The kernel reservation starts at `memory_base`, not `__kernel_start`. This
deliberately sacrifices the 512 KiB between the RAM base and the kernel load
address, which holds QEMU's boot stub. Reclaiming it would gain little and risk
overwriting something still in use.

The DTB is reserved because `kern_init()`'s `device_tree` holds pointers into it
for the whole life of the kernel.

`pmm_reserve_range()` clips the requested range to the managed region, aligns the
start down and the end up — so a partially covered page is fully reserved, never
partially — and marks each page. `pmm_reserve_page()` is idempotent: it returns
without adjusting the counters if the page is already used, so overlapping
reservations cannot corrupt the accounting.

### Padding bits

```c
uint64_t bitmap_bit_count = g_pmm.bitmap_bytes * 8ULL;

for (uint64_t page = g_pmm.page_count; page < bitmap_bit_count; page++) {
	pmm_bitmap_set_used(page);
}
```

The bitmap's last byte may contain bits beyond the final real page. These are
marked used directly with `pmm_bitmap_set_used()` rather than
`pmm_reserve_page()`, because the latter would reject an index at or beyond
`page_count` — and because these bits must not affect the free/used counters,
which describe real pages only.

## Allocation

```c
bool pmm_allocate_page(uint64_t *physical_address);
```

Search policy is a **rotating first-fit**:

```c
for (checked = 0; checked < page_count; checked++) {
	page_index = (next_hint + checked) % page_count;
	if (pmm_bitmap_is_used(page_index)) continue;
	/* take it */
}
```

`next_hint` starts at 0 and is set to `page_index + 1` after each success, so
consecutive allocations walk forward through memory rather than repeatedly
rescanning the reserved region at the start. The modulo makes the scan wrap, so
a full pass is always performed before reporting exhaustion.

`pmm_free_page()` moves `next_hint` backward if the freed page precedes it, so a
just-freed page is found quickly.

Every allocated page is **zeroed** before being returned:

```c
memset((void *)address, 0, PMM_PAGE_SIZE);
```

This is a contract, not an optimization detail, and two callers depend on it:
`vmm_allocate_table()` requires a zeroed page so every descriptor in a new
translation table is invalid, and `kcalloc()` benefits from it indirectly. It
works because RAM is identity-mapped, so the physical address is a usable
pointer.

Allocation fails — returning `false` without writing `*physical_address` — if
the PMM is uninitialized, the pointer is null, `free_page_count` is zero, or the
scan completes without finding a free page.

## Freeing

```c
bool pmm_free_page(uint64_t physical_address);
```

Validation, in order:

1. The PMM is initialized.
2. The address is page-aligned.
3. `pmm_address_to_index()` succeeds — the address is inside the managed range.
4. The page is currently marked used.

Check 4 is the double-free defence: freeing an already-free page returns `false`
and changes nothing. It cannot, however, distinguish a genuine double free from
an attempt to free a permanently reserved page, and it cannot detect freeing a
page that is still mapped — that discipline belongs to the caller.

## Accounting

| Field | Meaning |
| --- | --- |
| `page_count` | Total pages in the managed range |
| `free_page_count` | Pages with a clear bit |
| `used_page_count` | Pages with a set bit, allocated and reserved together |
| `next_hint` | Where the next allocation scan begins |

`free_page_count + used_page_count == page_count` is invariant. It is maintained
by `pmm_reserve_page()`, `pmm_allocate_page()` and `pmm_free_page()`, and
deliberately *not* by the padding-bit loop, which touches bits outside the real
page range.

`pmm_get_page_count()`, `pmm_get_free_page_count()` and
`pmm_get_used_page_count()` expose these; `pmm_dump()` prints them and, with `-v`, the
RAM range, bitmap location and kernel bounds.

On the observed boot: 131072 total, 406 used (146 kernel and below, 4 bitmap,
256 DTB), 130666 free.

## Ownership contracts

- `pmm_allocate_page()` transfers ownership of one zeroed physical page to the
  caller. The PMM will not hand it out again until it is freed.
- The caller must eventually call `pmm_free_page()` with the **exact** address
  it received. There is no partial free and no size parameter.
- The caller must ensure the page is not reachable through any live translation
  before freeing it. The PMM cannot check this, and handing out a still-mapped
  page creates an aliasing bug that will surface arbitrarily far away.
- Reserved pages are never owned by anyone and can never be freed.
- The PMM does not track *who* owns a page. There is no reference count and no
  owner field, so a page cannot be shared between two independent lifetimes.

## Overflow safety

`pmm_add()` and `pmm_align_up()` both return `bool` and refuse to produce a
wrapped result. They are used for every computation on externally supplied
values — the RAM region base and size, the DTB base and size — so a malformed
Device Tree causes a clean `pmm_init()` failure rather than a nonsensical
range. `pmm_align_up()` additionally rejects a non-power-of-two alignment.

## Concurrency and interrupt context

The PMM takes no locks and is not reentrant.

- `pmm_init()` must be called exactly once, before any allocation.
- `pmm_allocate_page()` and `pmm_free_page()` must **not** be called from
  interrupt context. Both perform read-modify-write on the bitmap and update
  three counters non-atomically.
- The accessors and `pmm_dump()` read shared state without synchronization.

Safety today rests on single-core execution with IRQs masked during
initialization.

## Current limitations

- **Only the first memory region is managed.** `pmm_init()` uses
  `platform->memory_regions[0]` and ignores the rest. On a machine with several
  RAM banks the others would be identity-mapped by the VMM but never allocatable.
  QEMU `virt` presents one contiguous region, so this is invisible today.
- **Single-page granularity.** There is no contiguous multi-page allocator, so a
  DMA buffer requiring physically contiguous memory cannot be obtained.
- **No reference counting**, so a physical page cannot be shared.
- **No allocation ownership tracking**, so a leak cannot be attributed.
- **Reservations are permanent** and there is no API to release one.
- **The scan is O(page_count)** in the worst case, with no free-list or
  buddy structure.
- **Zeroing on every allocation** costs a full page memset even when the caller
  will overwrite the page immediately.
- **Freeing a still-mapped page is not detected.**

## Source files

- [`vm/pmm.c`](../../vm/pmm.c)
- [`vm/pmm.h`](../../vm/pmm.h)
- [`platform/platform.h`](../../platform/platform.h)
- [`makedefs/linker.ld`](../../makedefs/linker.ld)

## Related documentation

- [VM overview](overview.md)
- [Virtual memory](virtual-memory.md)
- [Kernel virtual arena](kernel-virtual-arena.md)
- [Heap](../kern/heap.md)
- [Memory map](../memory-map.md)
- [Hardware discovery](../platform/hardware-discovery.md)
