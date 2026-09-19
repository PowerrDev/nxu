/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/vm_map.h
 *
 * General-purpose anonymous memory: unlike vm/vm_shm.h's opaque, id-based,
 * 16MB-capped handle meant for cross-process sharing, this is a private,
 * per-address-space region list a task uses for its own heap growth and
 * thread stacks. Eagerly backed (every page is allocated and mapped up
 * front, exactly like vm_shm) rather than demand-paged -- there is no
 * page-fault handler in this kernel yet, and adding one is out of scope
 * for this milestone.
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
#include <mach/machine/vm_param.h>

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
