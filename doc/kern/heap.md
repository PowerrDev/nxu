# Kernel Heap

The NXU heap provides byte-granular kernel allocation through `kmalloc()`,
`kcalloc()` and `kfree()`. It is a two-tier allocator: small requests are carved
out of page-local block arenas, and large requests are satisfied by the
[kernel virtual arena](../vm/kernel-virtual-arena.md).

## Two tiers

```text
kmalloc(size)
     |
     +-- align size up to 16 bytes
     |
     +-- aligned size <= 4016 ? -- yes --> small allocation
     |                                     page-local block allocator
     |                                     backed by one PMM page
     |                                     identity-mapped
     |
     +-------------------------- no ----> large allocation
                                           vm_kern_allocate()
                                           N physical pages, one
                                           contiguous virtual range
```

The threshold is not a tuning constant. It is the largest payload a single 4 KiB
page can hold once both headers are accounted for:

```c
maximum_size = PMM_PAGE_SIZE
             - heap_page_header_size()    /* 32 */
             - heap_block_header_size();  /* 48 */
```

On the current build that is **4016 bytes**, and `heap_dump()` prints the
computed value (with `-v`) rather than a literal. Code that needs the threshold should
derive it the same way; changing either header structure changes it.

The comparison uses the *aligned* size, so a request of 4010 bytes rounds to
4016 and stays small, while 4017 rounds to 4032 and becomes a large allocation.

## Alignment

Every allocation is 16-byte aligned. This is the AArch64 stack and structure
alignment requirement and is sufficient for any type NXU uses.

The guarantee is structural rather than enforced per allocation:

- Page headers are 32 bytes and block headers 48 bytes, both already multiples
  of 16 after `heap_align_up()`.
- A page begins at a 4 KiB boundary, so the first block header begins at
  offset 32 and its payload at offset 80 — both 16-aligned.
- `kmalloc()` rounds every request up to a multiple of 16 before splitting, so
  each subsequent header and payload stays 16-aligned.
- Large allocations come from `vm_kern_allocate()`, which returns page-aligned
  addresses.

## Data structures

### Page header

```c
struct heap_page {
	uint64_t     magic;             /* HEAP_PAGE_MAGIC */
	heap_page_t *next;              /* next arena page, or NULL */
	uint64_t     allocation_count;  /* live allocations in this page */
	uint64_t     reserved;
};
```

32 bytes, at offset 0 of every small arena page. `allocation_count` is what
makes empty-page reclamation possible: it is the only per-page liveness
information, since blocks are not tracked globally.

### Block header

```c
struct heap_block {
	uint64_t      magic;      /* HEAP_BLOCK_MAGIC */
	uint64_t      size;       /* payload bytes, excluding this header */
	heap_block_t *next;       /* next block in this page */
	heap_block_t *previous;   /* previous block in this page */
	uint64_t      flags;      /* bit 0: HEAP_BLOCK_FREE */
	uint64_t      reserved;
};
```

48 bytes, immediately preceding its payload. The list is per-page and in address
order — `next` and `previous` are always physically adjacent blocks — which is
what makes coalescing a pointer comparison rather than a search.

`size` counts payload only. A block therefore occupies `48 + size` bytes.

### Magic values

| Constant | Value | ASCII |
| --- | --- | --- |
| `HEAP_PAGE_MAGIC` | `0x4845415050414745` | `HEAPPAGE` |
| `HEAP_BLOCK_MAGIC` | `0x48454150424C4F43` | `HEAPBLOC` |

These are integrity checks, not security measures. They catch a pointer that was
never returned by the heap, a pointer into the middle of a block, and a block
whose header has been overwritten by an underflow. `heap_merge_with_next()`
zeroes the absorbed block's magic so a stale pointer to it fails validation.

### Large allocation record

```c
typedef struct {
	void  *address;
	size_t requested_size;
	uint64_t page_count;
	bool   active;
} heap_large_allocation_t;
```

A fixed array of `HEAP_MAX_LARGE_ALLOCATIONS` (**256**) records lives in
`g_heap`. Large allocations need this table because their pages carry no
in-band header: the payload starts at the very first byte of the returned
range, so there is nowhere to store metadata without either wasting a page or
misaligning the result.

`requested_size` is retained because `vm_kern_free()` requires the exact size
that was passed to `vm_kern_allocate()`.

## Page layout

```text
page base (4 KiB aligned)
+----------------------------+  offset 0
| heap_page_t                |  32 bytes
|   magic, next,             |
|   allocation_count         |
+----------------------------+  offset 32
| heap_block_t               |  48 bytes
|   magic, size, next, prev, |
|   flags = in use           |
+----------------------------+  offset 80
| allocation payload         |  16-byte aligned
|                            |
+----------------------------+
| heap_block_t               |  48 bytes
|   flags = HEAP_BLOCK_FREE  |
+----------------------------+
| free payload               |
|                            |
+----------------------------+  offset 4096
```

A freshly created page holds exactly one free block of 4016 bytes.

## Allocation

### Small path

1. Round the request up to 16 bytes.
2. `heap_find_free_block()` walks every page, and within each page every block,
   returning the **first** free block whose `size` is at least the request. This
   is first-fit across a first-fit page list.
3. If no page has a suitable block, `heap_create_page()` obtains one PMM page,
   `heap_append_page()` links it at the tail, and its single free block becomes
   the candidate.
4. `heap_split_block()` divides the block if the remainder is worth keeping.
5. Clear `HEAP_BLOCK_FREE`, increment the page's `allocation_count` and the
   global counters, and return `heap_block_payload(block)`.

### Splitting

```c
if (block->size < requested_size ||
    block->size - requested_size < header_size + HEAP_MIN_SPLIT) {
	return;
}
```

A block is split only if the leftover can hold a 48-byte header plus at least
`HEAP_MIN_SPLIT` (16) usable bytes — that is, at least 64 spare bytes. Otherwise
the whole block is handed out and the surplus becomes internal fragmentation.
Splitting more aggressively would create free blocks too small to ever satisfy a
request while still costing a full header.

The new block is spliced into the address-ordered list and marked free; the
original block's `size` is reduced to the request.

### Large path

`heap_allocate_large()`:

1. Find a free record; fail if all 256 are in use.
2. Compute the page count via `heap_size_to_pages()` (round up to 4 KiB).
3. `vm_kern_allocate(size, VMM_PROTECTION_READ_WRITE, &address)`.
4. Fill the record and update both the global and large-specific counters.

Large allocations are always mapped read-write Normal memory. There is no way to
request different protection through `kmalloc()`.

Note the page rounding: a 12621-byte request becomes four pages (16384 bytes).
The record stores the requested size for `vm_kern_free()`, and the page count
for accounting; both must agree with what `vm_kern` computes, which they do
because both use the same round-up.

### `kcalloc()`

```c
if (count > (size_t)-1 / size) {
	return 0;
}
```

Rejects a `count * size` product that would overflow, then calls `kmalloc()` and
zeroes the result. Note it zeroes exactly `total_size` bytes, not the full
block, so any surplus from splitting or page rounding is left with whatever the
PMM's page zeroing put there — which is zero for a fresh page but not
necessarily for a reused block.

## Freeing

`kfree()` classifies the pointer before touching anything:

```text
kfree(address)
     |
     +-- heap_find_large_record(address) found? -- yes --> heap_free_large()
     |
     +-- vm_kern_contains(address)? ------------- yes --> return false
     |                                                    (see below)
     |
     +-- treat as a small allocation
              page = address & ~0xFFF
              validate page->magic
              validate the payload lies inside the page
              block = address - 48
              validate block->magic, payload identity, not already free
              validate accounting cannot underflow
              mark free, coalesce
              if page != head && page->allocation_count == 0: unlink and free
```

### Double-free handling

The `vm_kern_contains()` check is the double-free defence for large
allocations, and it is deliberately placed *before* any dereference. After a
large allocation is freed its pages are unmapped, so the address no longer
translates — reading a `heap_page_t` header from it would fault. Any arena
address that does not match an active record is therefore rejected outright.
This catches:

- a second `kfree()` of the same large allocation;
- a pointer into the middle of a large allocation;
- any unrelated arena address.

Small double-frees are caught differently, by `heap_block_is_free(block)`:
freeing an already-free block returns `false` without modifying anything.

Neither case panics. `kfree()` returns `bool`, and `false` means "this pointer
was rejected and nothing was changed".

### Coalescing

`heap_coalesce()` merges in two directions:

```c
heap_merge_with_next(block);

if (block->previous != 0 && heap_block_is_free(block->previous)) {
	block = block->previous;
	heap_merge_with_next(block);
}
```

Merging absorbs the successor's 48-byte header into the predecessor's `size`, so
repeatedly splitting and coalescing does not lose space. Because the list is
address-ordered and per-page, adjacency is exactly list adjacency; no free-list
search is needed.

Coalescing never crosses a page boundary, so a request larger than the biggest
free block in any single page always requires a new page even if the total free
space is sufficient.

### Page retention and release

```c
if (page != g_heap.head && page->allocation_count == 0ULL) {
	return heap_unlink_page(page);
}
```

The **first** arena page is never released. Retaining it means a heap that has
been completely emptied can still satisfy the next small allocation without a
round trip to the PMM, which is the common pattern during boot.

Every other page is released to the PMM as soon as its last allocation is freed.
`heap_unlink_page()` refuses to unlink the head, walks the list to find the
predecessor, unlinks, and calls `pmm_free_page()`. If the PMM rejects the free
it **restores the link** and returns `false`, so the page is never lost from the
list without also being returned.

## Accounting

| Field | Counts |
| --- | --- |
| `page_count` | Small arena pages currently retained |
| `allocation_count` | Live allocations, small and large together |
| `allocated_bytes` | For small: the block's `size`. For large: the *requested* size |
| `large_page_count` | Physical pages backing large allocations |
| `large_allocation_count` | Live large allocations |
| `large_allocated_bytes` | Requested bytes across live large allocations |

`heap_get_page_count()` returns `page_count + large_page_count`.

The two tiers account bytes differently, and the difference is worth knowing:
a small allocation contributes its rounded-up block size, while a large one
contributes the caller's exact request even though whole pages were consumed.
`allocated_bytes` is therefore neither pure demand nor pure footprint.

Every decrement is guarded. `heap_free_large()` and the small path in `kfree()`
both refuse to proceed if a counter would underflow, treating that as evidence
of corrupted metadata rather than silently wrapping.

## Ownership and lifetime

| Object | Owner | Lifetime |
| --- | --- | --- |
| Returned pointer | Caller | Until passed to `kfree()` |
| Block header | Heap | Until its page is released |
| Arena page | Heap; the PMM page is borrowed | Until the last allocation in it is freed (except the head page) |
| Large allocation record | Heap | Until `kfree()` |
| Large backing pages | `vm_kern`, which owns them on the heap's behalf | Until `kfree()` |

The caller owns only the payload bytes. Writing outside them corrupts a header
and will be caught, at best, by a magic check on some later call.

Small allocations are **identity-mapped physical memory**; the returned pointer
equals the physical address. Large allocations are **virtual only**; their
pointer is inside the arena and the underlying physical pages are scattered.
Nothing in the `kmalloc()` interface distinguishes them, and callers must not
assume either property.

## Failure behavior

`kmalloc()` and `kcalloc()` return `NULL`. `kfree()` returns `false`. Nothing
panics, and no diagnostic is printed. Specific causes:

| Cause | Result |
| --- | --- |
| `size == 0` | `NULL` |
| Heap not initialized | `NULL` |
| Size alignment would overflow | `NULL` |
| No free block and the PMM is exhausted | `NULL` |
| All 256 large records in use | `NULL` |
| `vm_kern_allocate()` fails (no run, no record, PMM exhausted) | `NULL` |
| `kcalloc()` multiplication overflow | `NULL` |
| `kfree(NULL)` | `false` |
| Unrecognized pointer, bad magic, already free | `false` |
| Accounting would underflow | `false` |
| `vm_kern_free()` rejects the range | `false` |

One consequence worth noting: `heap_find_free_block()` returns `NULL` both when
no suitable block exists **and** when it encounters a bad magic value. `kmalloc()`
cannot distinguish the two, so metadata corruption manifests as an extra page
allocation rather than an error.

## Concurrency and interrupt context

The heap takes **no locks** and is **not reentrant**.

- `kmalloc()`, `kcalloc()` and `kfree()` must **not** be called from interrupt
  context. They walk and mutate the page and block lists, and they call into the
  PMM and `vm_kern`, which are equally unsynchronized.
- `heap_init()` must be called exactly once, before any allocation, and refuses
  a second call.
- `heap_dump()` walks every list and must not run concurrently with an
  allocation. It is only called from `kern_init()`.

Safety today rests entirely on NXU being single-core with all allocation
happening before IRQs are unmasked. A scheduler, a second core, or any
allocating interrupt handler would require real locking.

## Current limitations

- **Maximum 256 concurrent large allocations**, from a fixed array. The
  251st-and-beyond concurrent large allocation fails even with free memory and
  free arena space.
- **No `krealloc()`.**
- **No size query.** A caller cannot ask how large an allocation is.
- **No alignment control.** Everything is 16-byte aligned and nothing more is
  available.
- **No protection control** for large allocations; always read-write Normal.
- **Small-allocation search is O(pages x blocks).** There is no free list, no
  size class and no lookaside cache.
- **No cross-page coalescing**, so free space fragments per page.
- **`heap_find_large_record()` is a linear scan** of all 256 records on every
  `kfree()`.
- **No poisoning or redzones.** A payload overflow silently corrupts the next
  header until a magic check happens to notice.
- **The head page is never released**, so the heap retains one physical page
  permanently once initialized.
- **No shrink or compaction.**

## Source files

- [`kern/heap.c`](../../kern/heap.c)
- [`kern/heap.h`](../../kern/heap.h)
- [`vm/vm_kern.c`](../../vm/vm_kern.c)
- [`vm/pmm.c`](../../vm/pmm.c)

## Related documentation

- [VM overview](../vm/overview.md)
- [Kernel virtual arena](../vm/kernel-virtual-arena.md)
- [Physical memory](../vm/physical-memory.md)
- [Core kernel initialization](initialization.md)
- [Memory map](../memory-map.md)
