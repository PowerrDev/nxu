# Virtual Memory Overview

NXU separates memory management into four layers. Each has exactly one owner,
one unit of work, and one kind of thing it hands out. The separation is the
point: a bug in one layer should be diagnosable without reasoning about the
others.

```text
+---------------------------------------------------------------+
| heap            kern/heap.c                                   |
|                 byte-granular allocation                      |
|                 kmalloc / kcalloc / kfree                     |
+---------------------------------------------------------------+
        |                                        |
        | large allocations                      | small allocations
        v                                        |
+-------------------------------------------+    |
| vm_kern         vm/vm_kern.c              |    |
|                 kernel virtual address    |    |
|                 range allocation          |    |
+-------------------------------------------+    |
        |                                        |
        v                                        |
+---------------------------------------------------------------+
| VMM             vm/vmm.c                                      |
|                 translation-table mappings                    |
|                 virtual page -> physical page + attributes    |
+---------------------------------------------------------------+
        |
        v
+---------------------------------------------------------------+
| PMM             vm/pmm.c                                      |
|                 physical page ownership                       |
|                 4 KiB pages, allocated and freed              |
+---------------------------------------------------------------+
```

Note the asymmetry: the heap's **small** path reaches past `vm_kern` and the VMM
directly to the PMM. This is legitimate rather than a layering violation,
because the whole of RAM is identity-mapped: a physical page from the PMM is
already accessible at that address, so no mapping step is needed. It is also the
single most important thing that changes when NXU moves to a higher-half
kernel.

## Ownership

| Resource | Owned by | Allocated through | Released through |
| --- | --- | --- | --- |
| Physical pages | PMM | `pmm_allocate_page()` | `pmm_free_page()` |
| Translation-table entries | VMM | `vmm_map_page()` | `vmm_unmap_page()` |
| Translation-table *pages* | VMM (obtained from PMM) | implicitly, during mapping | never |
| Kernel virtual ranges | `vm_kern` | `vm_kern_allocate()` | `vm_kern_free()` |
| Allocation metadata | each layer, statically | — | — |
| Pointers returned to callers | the caller | `kmalloc()` / `vm_kern_allocate()` | `kfree()` / `vm_kern_free()` |

Two ownership rules deserve emphasis, because violating either produces a bug
that surfaces far from its cause:

- **A physical page may be owned by exactly one thing at a time.** When
  `vm_kern_free()` unmaps a page it immediately calls `pmm_free_page()` with the
  address the VMM returned. Conversely, `kern_test_live_vmm()` deliberately
  refuses to free a physical page it could not unmap: a page that is still
  reachable through a live descriptor must never return to the free pool.
- **The VMM does not own physical pages it maps.** `vmm_map_page()` takes a
  physical address and installs a descriptor; it does not allocate, and
  `vmm_unmap_page()` does not free. It *returns* the old physical address
  precisely so the caller can decide what to do with it. The one exception is
  the pages the VMM consumes for translation tables themselves, which it
  allocates from the PMM and never releases.

## Layer responsibilities

### PMM — physical page ownership

Tracks which 4 KiB physical pages are in use, using one bit per page. Knows
nothing about virtual addresses, permissions or mappings. Guarantees every
allocated page is zeroed. Permanently reserves the kernel image, its own bitmap
and the DTB.

See [Physical memory](physical-memory.md).

### VMM — translation-table mappings

Owns the translation tables and the MMU control registers. Builds the initial
identity map, enables stage-1 translation, and thereafter installs, queries,
reprotects and removes individual 4 KiB mappings. Knows nothing about which
virtual addresses are "free" — an unmapped address and an unallocated one are
indistinguishable to it.

See [Virtual memory](virtual-memory.md) and
[Translation tables](translation-tables.md).

### `vm_kern` — kernel virtual address allocation

Owns a fixed 64 MiB virtual range and tracks which of its 16384 page slots are
in use. Composes a virtually contiguous range out of physically scattered pages
by calling the PMM for each page and the VMM to map each one. Knows nothing
about what the range will contain.

See [Kernel virtual arena](kernel-virtual-arena.md).

### Heap — byte-granular allocation

Turns pages into arbitrary-sized allocations. Splits and coalesces blocks inside
identity-mapped pages for small requests, and delegates to `vm_kern` for large
ones.

See [Heap](../kern/heap.md).

## Interfaces between layers

```text
vm_kern_allocate(size, protection, &address)
     |
     +--> vm_kern_find_free_run()        pick a run of page slots
     +--> vm_kern_reserve_run()          mark them used in the bitmap
     |
     +--> for each page:
     |        pmm_allocate_page(&physical)      PMM owns the page
     |        vmm_map_page(virtual, physical,   VMM owns the descriptor
     |                     NORMAL, protection)
     |
     +--> on any failure: roll back every mapping made so far,
          free each physical page, release the run
```

The rollback path is what keeps ownership consistent under failure. If the tenth
of twenty pages cannot be mapped, its physical page is freed immediately, the
nine already-mapped pages are unmapped and freed, and the whole run is released.
The caller sees a clean failure with no leaked pages and no dangling
descriptors.

## Initialization order

The layers must come up bottom-first, and each checks its own precondition:

| Order | Layer | Requires |
| --- | --- | --- |
| 1 | PMM | `platform_t` with at least one RAM region, and the DTB bounds |
| 2 | VMM | The PMM, to allocate its root and intermediate tables |
| 3 | `vm_kern` | `vmm_is_enabled()` — live mapping must work |
| 4 | Heap | The PMM (small tier) and `vm_kern` (large tier) |

See [Initialization](../initialization.md).

## Concurrency

No layer takes a lock. None is reentrant. None may be called from interrupt
context.

This is safe today only because NXU is single-core and every allocation
happens before `arm64_enable_irqs()` runs. It is a property of the current
initialization order, not of the code, and it is the first thing that breaks
under a scheduler or SMP.

## Current limitations

- Only `platform->memory_regions[0]` is managed by the PMM.
- TTBR1 permanently owns the higher-half kernel; TTBR0 owns the active user address space.
- Translation-table pages are never reclaimed.
- Existing block descriptors cannot be split, so live mapping only works in
  regions the identity map left at page granularity, or in previously unmapped
  regions such as the arena.
- No page fault handling: a fault is fatal.
- No demand paging, no copy-on-write, no swapping, no memory pressure handling.
- No physical page reference counting, so a page cannot be shared by two
  mappings with independent lifetimes.

## Source files

- [`vm/pmm.c`](../../vm/pmm.c)
- [`vm/pmm.h`](../../vm/pmm.h)
- [`vm/vmm.c`](../../vm/vmm.c)
- [`vm/vmm.h`](../../vm/vmm.h)
- [`vm/vmm_internal.h`](../../vm/vmm_internal.h)
- [`vm/vmm_tables.c`](../../vm/vmm_tables.c)
- [`vm/vmm_ttbr1.c`](../../vm/vmm_ttbr1.c)
- [`vm/vmm_debug.c`](../../vm/vmm_debug.c)
- [`vm/vm_kern.c`](../../vm/vm_kern.c)
- [`vm/vm_kern.h`](../../vm/vm_kern.h)
- [`kern/heap.c`](../../kern/heap.c)

## Related documentation

- [Physical memory](physical-memory.md)
- [Virtual memory](virtual-memory.md)
- [Kernel virtual arena](kernel-virtual-arena.md)
- [Translation tables](translation-tables.md)
- [Heap](../kern/heap.md)
- [Memory map](../memory-map.md)
- [Architecture](../architecture.md)
