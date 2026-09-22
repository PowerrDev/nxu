/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/vm_map.h
 *
 * General-purpose anonymous memory: unlike vm/vm_shm.h's opaque, id-based,
 * 16MB-capped handle meant for cross-process sharing, this is a private,
 * per-address-space region list a task uses for its own heap growth,
 * thread stacks and (with the initial user stack) demand-zero memory.
 *
 * Where the port resolves user page faults (VM_DEMAND_PAGING, see
 * <kern/machine/vm_param.h>), a region only reserves address space: each
 * page is allocated, zero-filled and mapped by vm/vm_fault.h the first time
 * something touches it. Where it does not (i386), every page is allocated
 * and mapped up front, exactly like vm_shm.
 *
 * Regions are tracked in a sorted, non-overlapping singly-linked list
 * (vm_address_space_t's mmap_entries) so vm_map_free can split, shrink, or
 * remove exactly the range asked for. Placement is bump-allocated
 * (mmap_next_va only ever grows) -- vm_map_free does not make freed VA
 * space available for reuse; a full VMA allocator with gap reuse is future
 * work, not needed to unblock thread stacks / heap growth for this
 * milestone.
 */

#ifndef NXU_VM_VM_MAP_H
#define NXU_VM_VM_MAP_H

#include <vm/address_space.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Reserved per-process window for anonymous mappings, disjoint from
 * vm/vm_shm.h's VM_SHM_BASE window and from kern/loader/elf.c's
 * LOADER_USER_STACK_BASE/TOP.
 */
#include <kern/machine/vm_param.h>

/* Per-call cap, eager-backed like vm_shm's own cap, just larger since this
 * is meant for general-purpose heap/stack use rather than one framebuffer. */
#define VM_MAP_MAX_BYTES (64ULL * 1024ULL * 1024ULL)

typedef struct vm_map_entry {
	struct vm_map_entry *next;
	uint64_t start_va;
	uint64_t end_va; /* exclusive */
	vm_user_protection_t protection;
} *vm_map_entry_t;

/*
 * Allocates size_bytes worth of physical pages (zeroed, per
 * pmm_allocate_page) and maps them at a freshly bump-allocated VA in space,
 * recording one new region. On any failure space is left unchanged.
 */
bool vm_map_anon(
	vm_address_space_t *space,
	uint64_t size_bytes,
	vm_user_protection_t protection,
	uint64_t *out_va
);

/*
 * Records [start_va, end_va) as a region at a caller-chosen address without
 * mapping any page (the initial user stack is reserved this way). Both
 * bounds must be page-aligned, the range non-empty, below
 * VM_MAX_USER_ADDRESS and clear of every existing region. On a port without
 * demand paging the caller has to map the pages itself.
 */
bool vm_map_reserve(
	vm_address_space_t *space,
	uint64_t start_va,
	uint64_t end_va,
	vm_user_protection_t protection
);

/*
 * Like vm_map_lookup, and also reports the region's protection -- what the
 * fault handler needs to decide whether an untouched page may be populated.
 */
bool vm_map_region_at(
	const vm_address_space_t *space,
	uint64_t va,
	vm_user_protection_t *out_protection,
	uint64_t *out_start_va,
	uint64_t *out_end_va
);

/*
 * vm_map_region_at for a caller that already holds space->lock: the fault handler
 * checks that a page is still inside a region and installs it in one critical
 * section, so an munmap on another CPU cannot slip between the two.
 */
bool vm_map_region_at_locked(
	const vm_address_space_t *space,
	uint64_t va,
	vm_user_protection_t *out_protection
);

/*
 * Copies parent's region list and placement cursor into child (an address
 * space with no regions of its own yet) so a forked child sees the same
 * anonymous mappings. Pages are not touched: vm_address_space_fork shares
 * them copy-on-write, and untouched pages stay untouched in both.
 */
bool vm_map_fork(
	const vm_address_space_t *parent,
	vm_address_space_t *child
);

/*
 * Unmaps and frees the pages covering [va, va + size_bytes) and updates the
 * region list -- removing, shrinking, or splitting the one tracked region
 * that fully covers the range. Fails without changing anything if no single
 * tracked region fully covers [va, va + size_bytes); this call cannot split
 * work across more than one existing region.
 */
bool vm_map_free(
	vm_address_space_t *space,
	uint64_t va,
	uint64_t size_bytes
);

/*
 * True and (if non-null) fills out_start_va/out_end_va with the tracked
 * region containing va, false if va falls outside every vm_map_anon'd
 * region in space.
 */
bool vm_map_lookup(
	const vm_address_space_t *space,
	uint64_t va,
	uint64_t *out_start_va,
	uint64_t *out_end_va
);

/*
 * Unmaps and frees every remaining vm_map_anon'd region in space. Called
 * from task_terminate once every thread of the task has already been torn
 * down -- not safe to call while another thread of the same task might
 * still be running.
 */
void vm_map_destroy_all(vm_address_space_t *space);

#endif
