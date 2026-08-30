# Kernel Virtual Arena (`vm_kern`)

`vm_kern` allocates **kernel virtual address ranges**. It sits between the VMM
and the heap: given a size, it finds a run of free virtual pages, obtains a
physical page for each one from the PMM, maps them through the VMM, and returns
a virtually contiguous pointer.

The physical pages behind such a range are generally **not** contiguous. That is
the entire reason the layer exists — it lets the kernel hand out a 4 MiB
contiguous buffer without needing 4 MiB of contiguous RAM.

## Reserved range

```c
#define VM_KERN_BASE 0x0000001000000000ULL
#define VM_KERN_SIZE 0x0000000004000000ULL   /* 64 MiB */
#define VM_KERN_END  (VM_KERN_BASE + VM_KERN_SIZE)
```

| Property | Value |
| --- | --- |
| Base | `0x1000000000` (64 GiB) |
| Size | 64 MiB |
| End | `0x1004000000` |
| Page slots | 16384 |
| Bitmap size | 2048 bytes |

The base is chosen to sit far above anything the identity map can produce on the
QEMU `virt` machine. Two properties follow:

- `vm_kern_contains()` is a simple range test, and an arena pointer is
  immediately recognizable — which the heap relies on to classify a `kfree()`
  argument before dereferencing it.
- The range is untouched by the identity map, so no block descriptor obstructs
  the L3 walk. This matters because the VMM cannot split existing blocks; see
  [Virtual memory](virtual-memory.md).

The whole range fits inside the 39-bit virtual address space (512 GiB) that
`TCR_EL1.T0SZ` configures.

## State

```c
typedef struct {
	uint8_t bitmap[VM_KERN_BITMAP_SIZE];             /* 2048 bytes */
	vm_kern_allocation_t allocations[VM_KERN_MAX_ALLOCATIONS];
	uint64_t used_pages;
	uint64_t allocation_count;
	bool     initialized;
} vm_kern_state_t;
```

All of it is a single file-scope `static`, so the arena consumes no dynamic
memory for its own metadata. It lives in `.bss` and is therefore zeroed by
`_start` before `vm_kern_init()` even runs.

### Bitmap

One bit per page slot, with the same convention as the PMM: 0 free, 1 used.
Indexing is identical:

```text
page index  = (virtual_address - VM_KERN_BASE) / PMM_PAGE_SIZE
bitmap byte = page_index >> 3
bitmap bit  = page_index & 7
```

and in reverse, `vm_kern_page_address(index) = VM_KERN_BASE + index * 4096`.

### Allocation records

```c
typedef struct {
	uint64_t start_page;
	uint64_t page_count;
	bool     active;
} vm_kern_allocation_t;
```

A fixed array of `VM_KERN_MAX_ALLOCATIONS` (**256**) records. The bitmap alone
would say which pages are used but not where one allocation ends and the next
begins, so `vm_kern_free()` could not verify that the caller is releasing a whole
allocation rather than a slice of one. The records supply exactly that.

Records are matched on `(start_page, page_count)`, which is why the size passed
to `vm_kern_free()` must match the original request.

## Initialization

```c
bool vm_kern_init(void);
```

Requires `vmm_is_enabled()` — every allocation calls `vmm_map_page()`, which
only works with translation live. Refuses a second call.

Zeroes the entire state and marks it initialized. **No memory is consumed and no
page is mapped.** The arena is purely a reservation until something allocates
from it.

**Return:** `true` on success; `false` if already initialized or if the MMU is
not enabled.

## Allocation

```c
bool vm_kern_allocate(size_t size, vmm_protection_t protection, void **address);
```

Rounds `size` up to whole pages, finds a free run, reserves it, and then maps it
page by page.

```text
vm_kern_allocate(size, protection, &address)
     |
     +--> *address = 0
     |
     +--> vm_kern_size_to_pages(size)
     |       reject 0, reject > 16384 pages
     |
     +--> vm_kern_find_free_record()
     |       fail if all 256 records are active
     |
     +--> vm_kern_find_free_run(page_count)
     |       first-fit scan from slot 0
     |       fail if no run is long enough
     |
     +--> vm_kern_reserve_run(start, count)
     |       re-verify every slot is free, then set every bit
     |
     +--> for index in 0 .. count-1:
     |        pmm_allocate_page(&physical)
     |          on failure -> rollback
     |        vmm_map_page(page_address(start+index), physical,
     |                     VMM_MEMORY_NORMAL, protection)
     |          on failure -> free this physical page, then rollback
     |
     +--> record = {start, count, active}
          allocation_count++
          *address = page_address(start)
```

Note the order: the free record is claimed **before** the run is searched, so a
successful search is never wasted on a full record table.

`*address` is set to `NULL` on entry, so a caller that ignores the return value
still sees a null pointer rather than a stale one.

### First-fit

`vm_kern_find_free_run()` scans from slot 0 and returns the first run of
`page_count` consecutive free slots:

```c
for (page = 0; page < VM_KERN_PAGE_COUNT; page++) {
	if (vm_kern_page_used(page)) { run_length = 0; continue; }
	if (run_length == 0) { run_start = page; }
	if (++run_length == page_count) { *start_page = run_start; return true; }
}
```

Always starting at slot 0 means a freed range is reused immediately by the next
same-or-smaller request. `kern_test_vm_kern()` asserts exactly this: it
allocates A, allocates B, frees A, allocates a same-sized range, and requires
that the new range lands at A's old address.

The trade-off is that allocation is O(16384) bit tests in the worst case, and
that the arena tends to fragment at the low end.

### Rollback

Every failure after `vm_kern_reserve_run()` unwinds completely:

```c
if (vm_kern_rollback_mappings(start_page, mapped_pages)) {
	(void)vm_kern_release_run(start_page, page_count);
}
```

`vm_kern_rollback_mappings()` walks the already-mapped pages backwards,
unmapping each and freeing its physical page. Only if that fully succeeds is the
virtual run released.

The conditional is deliberate. If a page could not be unmapped, releasing the
run would let those slots be allocated again while stale descriptors still point
at them. Leaving the run reserved leaks 4 KiB of virtual space but keeps the
address space consistent — the same trade-off `kern_test_live_vmm()` makes.

When `vmm_map_page()` fails, the physical page for *that* page is freed
immediately before the rollback runs, because it was never mapped and so is not
covered by `mapped_pages`.

**Return:** `true` with `*address` set; `false` with `*address` set to `NULL`.
Failure causes: not initialized, null `address`, zero or oversized size, all 256
records in use, no free run long enough, PMM exhausted, or a mapping failure.

## Freeing

```c
bool vm_kern_free(void *address, size_t size);
```

```text
vm_kern_free(address, size)
     |
     +--> validate: initialized, non-null
     +--> validate: VM_KERN_BASE <= address < VM_KERN_END
     +--> validate: page-aligned
     +--> vm_kern_size_to_pages(size) -> page_count
     +--> start_page = (address - VM_KERN_BASE) / 4096
     |
     +--> vm_kern_find_record(start_page, page_count)
     |       fail if no active record matches BOTH exactly
     |
     +--> validate every page first:
     |       vmm_query_page() succeeds
     |       memory_type == VMM_MEMORY_NORMAL
     |       (no page table is modified during this pass)
     |
     +--> for each page:
     |       vmm_unmap_page(page_address, &physical)
     |       pmm_free_page(physical)
     |
     +--> vm_kern_release_run(start, count)
     +--> memset(record, 0, sizeof(*record))
     +--> allocation_count--
```

### Exact address and size

Both must match the original allocation. The address must be the *first* page,
and the size must round to the same page count.

The size is required because the record table is keyed on
`(start_page, page_count)`. A caller passing a different size finds no matching
record and the free is rejected, changing nothing. This is a deliberate design
choice: the alternative — storing the size in-band — would either waste a page
or misalign the returned pointer, and the alternative of looking up by start
page alone would silently accept a wrong size.

`kfree()` satisfies this by storing `requested_size` in its large-allocation
record; see [Heap](../kern/heap.md).

### Validate-then-mutate

The full validation pass runs before any translation table is touched. If a page
in the middle of the range is not mapped, or was somehow mapped as Device
memory, the function fails having modified nothing — rather than leaving the
range half unmapped with no way to describe what happened.

**Return:** `true` on success; `false` on any validation failure. A `false`
return from the first phase guarantees nothing was changed; a `false` from the
unmapping loop does not, but by then the state is already corrupt.

## Queries

```c
bool     vm_kern_contains(const void *address);
uint64_t vm_kern_get_total_pages(void);      /* always 16384 */
uint64_t vm_kern_get_used_pages(void);
uint64_t vm_kern_get_free_pages(void);
uint64_t vm_kern_get_allocation_count(void);
void     vm_kern_dump(void);
```

`vm_kern_contains()` is a pure range test on `[VM_KERN_BASE, VM_KERN_END)`. It
returns `false` for `NULL` and, importantly, does **not** check whether the
address is actually allocated or mapped. The heap uses it precisely because it
is safe to call on an address that no longer translates.

## Ownership

| Resource | Owner |
| --- | --- |
| The virtual range | The caller, until `vm_kern_free()` |
| The bitmap slots | `vm_kern` |
| The allocation record | `vm_kern` |
| The backing physical pages | `vm_kern`, on the caller's behalf |
| Translation-table entries | The VMM |

The caller owns the *address range*, not the physical pages. It must never call
`pmm_free_page()` on anything behind an arena allocation, and must never unmap
an arena page itself; both are `vm_kern`'s to do at free time.

## Concurrency and interrupt context

`vm_kern` takes no locks and is not reentrant.

- `vm_kern_init()` must be called exactly once, after `vmm_init()`.
- `vm_kern_allocate()` and `vm_kern_free()` must **not** be called from
  interrupt context. Both mutate the bitmap and record table and call into the
  PMM and VMM, none of which is synchronized.
- `vm_kern_contains()` and the counters read shared state without
  synchronization; `vm_kern_contains()` reads only compile-time constants and is
  safe anywhere.

## Current limitations

- **Maximum 256 concurrent allocations**, from a fixed array, regardless of how
  much of the arena is free.
- **64 MiB fixed size**, set at compile time, with no way to grow.
- **No guard pages.** Allocations are placed adjacently, so an overrun runs
  straight into the next allocation's first page with no fault. Reserving one
  unmapped slot on each side would catch this at the cost of a slot per
  allocation; it is not done.
- **No page-table reclamation.** Freeing every page in a 2 MiB region leaves its
  L3 table allocated and its L2 descriptor valid. Over time the arena
  accumulates empty tables that are never returned to the PMM.
- **No synchronization** of any kind.
- **First-fit is O(page count)** with no free-list or best-fit structure, and
  tends to fragment the low end of the arena.
- **Fixed protection per allocation.** Protection is chosen at allocation time
  and there is no `vm_kern_protect()` to change it afterwards.
- **Normal memory only.** `vm_kern_allocate()` hard-codes `VMM_MEMORY_NORMAL`,
  so the arena cannot be used to map MMIO.
- **No partial free.** A range must be released exactly as it was allocated.
- **`vm_kern_free()` failing mid-unmap leaves the range inconsistent** with no
  recovery path.

## Source files

- [`vm/vm_kern.c`](../../vm/vm_kern.c)
- [`vm/vm_kern.h`](../../vm/vm_kern.h)
- [`vm/vmm.h`](../../vm/vmm.h)
- [`vm/pmm.h`](../../vm/pmm.h)

## Related documentation

- [VM overview](overview.md)
- [Virtual memory](virtual-memory.md)
- [Physical memory](physical-memory.md)
- [Translation tables](translation-tables.md)
- [Heap](../kern/heap.md)
- [Memory map](../memory-map.md)
