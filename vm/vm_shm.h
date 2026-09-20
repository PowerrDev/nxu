/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/vm_shm.h
 *
 * Shared-memory regions: physical pages one process allocates and maps into
 * its own address space, then hands a reference to (transferred through an
 * NXPC message -- see kern/ipc/ipc_types.h's IPC_KMSG_XFER_MEMORY) another
 * process, which maps the very same pages into its own address space too.
 * Built on vm_address_space_map_page() (already address-space-parameterized,
 * so writing into an inactive space's tables was already possible) plus a
 * new per-page reference count in vm/pmm.h so a page mapped into more than
 * one address space is not freed while any of them still has it mapped.
 *
 * Usage protocol: a region's own "references" count (vm_shm_reference/
 * vm_shm_release) tracks how many callers hold the *handle*, independent of
 * how many address spaces currently have it *mapped* (tracked instead by
 * pmm_page_retain/pmm_free_page against the underlying pages). Callers must
 * vm_shm_unmap_from() every address space they mapped it into before
 * releasing their last reference to the handle -- releasing the handle
 * first does not force-unmap anything (there is no list of mappings to walk
 * here), so a still-mapped page just keeps living, correctly refcounted by
 * pmm, but with no handle left to cleanly unmap it through this API later.
 */

#ifndef NXU_VM_VM_SHM_H
#define NXU_VM_VM_SHM_H

#include <vm/address_space.h>

#include <stdbool.h>
#include <stdint.h>

struct vm_shm_region;
typedef struct vm_shm_region *vm_shm_region_t;

#define VM_SHM_REGION_NULL ((vm_shm_region_t)0)
#define VM_SHM_MAX_BYTES (16ULL * 1024ULL * 1024ULL)

/*
 * Reserved per-process window for shared-memory mappings, bump-allocated by
 * the caller (see struct task's t_shm_next_va in kern/process/task.h) --
 * deliberately not a real VMA/region list, just enough for the handful of
 * shared regions (framebuffers) one process needs for this milestone.
 */
#include <kern/machine/vm_param.h>

/*
 * Allocates size_bytes worth of physical pages (zeroed, per
 * pmm_allocate_page) with one initial handle reference. Not mapped into any
 * address space yet -- see vm_shm_map_into.
 */
bool vm_shm_create(uint64_t size_bytes, vm_shm_region_t *regionp);

bool vm_shm_reference(vm_shm_region_t region);
void vm_shm_release(vm_shm_region_t region);

uint64_t vm_shm_size(vm_shm_region_t region);
uint64_t vm_shm_page_count(vm_shm_region_t region);

/*
 * Physical address of region's page `index` (< vm_shm_page_count(region)),
 * or 0 if out of range. Exposed for callers that need to reason about the
 * underlying pages directly -- e.g. pmm_page_refcount() in a test.
 */
uint64_t vm_shm_physical_page(vm_shm_region_t region, uint64_t index);

/*
 * Maps every page of region into space, starting at *next_va (a
 * caller-owned bump cursor -- e.g. &task->t_shm_next_va -- advanced past
 * the mapped range only on success). Retains a PMM reference on each page
 * for the duration of this mapping; see vm_shm_unmap_from.
 */
bool vm_shm_map_into(
	vm_address_space_t *space,
	uint64_t *next_va,
	vm_shm_region_t region,
	vm_user_protection_t protection,
	uint64_t *out_va
);

/*
 * Unmaps region's pages from space starting at va (as returned by
 * vm_shm_map_into) and releases the PMM reference that mapping held.
 * Does not touch region's own handle reference count.
 */
bool vm_shm_unmap_from(
	vm_address_space_t *space,
	uint64_t va,
	vm_shm_region_t region
);

#endif
