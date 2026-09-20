# Demand Paging and Copy-on-Write (arm64)

`VM_DEMAND_PAGING` (`kern/arm64/vm_param.h`) is 1 on arm64 and 0 on i386. Where
it is 1, user memory can be populated when it is first touched, and pages can be
shared between address spaces until one of them writes.

## Regions

A region is address space a task has claimed but not necessarily backed. The
list lives on the address space (`vm_map_entry`, sorted by address). Two things
create regions:

- `vm_map_anon()` — the `mmap` system call and thread stacks. Bump-allocated
  from the `VM_MAP_BASE` window. With demand paging it **allocates no pages**: a
  16 MiB region costs one list entry.
- `vm_map_reserve()` — a fixed range. The loader reserves the initial user
  stack this way (256 KiB, of which a process pays only for the pages it
  touches; without demand paging it stays 16 KiB and eager).

ELF segments are still mapped and filled eagerly: filling one lazily from the
file means sleeping inside the fault handler, which the non-preemptive kernel
cannot do yet.

## The fault resolver

`vm_fault_user(space, va, access)` (`vm/vm_fault.c`) decides whether an access
that could not complete is legitimate:

| Page is | Access | Action |
| --- | --- | --- |
| absent, inside a region whose protection allows the access | read/write/execute | allocate a page (the allocator returns it zeroed), map it with the region's protection |
| absent, outside every region | any | not resolvable |
| present, copy-on-write | write | `vm_address_space_cow_break()` |
| present, anything else | any | not resolvable (a genuine permission fault) |

It is called from three places:

- **EL0 aborts** (`exception_handle_user_fault`) — the ordinary case. The
  exception returns and the instruction retries.
- **EL1 data aborts on a user address** (`exception_handle_kernel_user_access`)
  — kernel code dereferencing a user pointer directly. It resolves against the
  *active* TTBR0 space, which is what the MMU walked, so kernel self-tests that
  run against a scratch space work.
- **`vm_copy_from_user` / `vm_copy_to_user`** — the kernel reaches user memory
  through a software page-table walk, so it has to resolve the fault the MMU
  would have raised.

An unresolvable fault becomes a signal (see
[Process control](../kern/process-control.md)).

## Copy-on-write

Bit 55 of a stage-1 descriptor is software-defined; NXU uses it as
`VMM_DESC_SW_COW`. `vm_address_space_fork()` walks the parent's L1→L2→L3 tables
and, for every mapped page:

- **private writable page** (not in the shared-memory window): rewrite it
  read-only with the COW bit **in both spaces** and take one more `pmm`
  reference;
- **read-only or executable page:** share the descriptor unchanged, one more
  reference;
- **shared-memory window page:** share it writable, one more reference — it
  must stay genuinely shared.

The first write to a COW page faults. `vm_address_space_cow_break()`:

- if `pmm_page_refcount()` is 1 (the other side is gone), makes the page
  writable in place;
- otherwise allocates a page, copies 4 KiB through the direct map, points this
  space's descriptor at the copy (writable, COW cleared), and drops its
  reference on the original.

`vm_address_space_query_page()` reports a COW page as
`VM_USER_PROTECTION_READ_ONLY` with `cow = true`, so code that writes through a
physical address after checking for `READ_WRITE` refuses it instead of
corrupting a page another process still sees.

## Teardown

`vm_address_space_release_pages()` drops one reference per mapped page and
clears the entries; `vm_address_space_destroy()` frees the L3/L2/L1 tables and
the root (deactivating the space first if it is the live one). `task_terminate()`
runs both, so page-table memory is now reclaimed on arm64.

## Limits

- The `pmm` shared-reference table has 4096 entries and is scanned linearly. It
  bounds how many pages can be shared at once (a few dozen forked processes) and
  makes each retain O(entries).
- Only anonymous memory is demand-paged; there is no file-backed mapping, no
  swap and no page reclaim under memory pressure.
- The i386 port does not implement any of this (`vm_fault_user` there always
  reports "unresolvable", and `fork` is unavailable).
