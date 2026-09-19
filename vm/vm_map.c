/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/vm_map.c
 *
 * Anonymous region lifecycle. See vm/vm_map.h for the usage protocol.
 *
 * space->lock (added in vm/address_space.h alongside vm_address_space_map_
 * page/unmap_page/query_page's own use of it) protects mmap_entries and
 * mmap_next_va here too. Every critical section below is a pure pointer/
 * list operation -- none of them call back into vm_address_space_map_page/
 * unmap_page/query_page while holding it, since those each take and
 * release the same lock internally per call and it is not reentrant.
 */

#include <vm/vm_map.h>

#include <kern/lock.h>
#include <kern/memory/heap.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>

bool vm_map_anon(
	vm_address_space_t *space,
	uint64_t size_bytes,
	vm_user_protection_t protection,
	uint64_t *out_va
)
{
	if (out_va != 0) *out_va = 0ULL;

	if (
		space == 0 ||
		out_va == 0 ||
		size_bytes == 0ULL ||
		size_bytes > VM_MAP_MAX_BYTES
	) {
		return false;
	}

	uint64_t page_count = (size_bytes + PMM_PAGE_SIZE - 1ULL) / PMM_PAGE_SIZE;
	uint64_t span = page_count * PMM_PAGE_SIZE;

	nxu_spin_lock(&space->lock);

	uint64_t base_va = space->mmap_next_va;
	if (base_va < VM_MAP_BASE) base_va = VM_MAP_BASE;

	if (
		span > VM_MAP_WINDOW_SIZE ||
		base_va - VM_MAP_BASE > VM_MAP_WINDOW_SIZE - span
	) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	/* Reserve the range now, before any page is mapped, so a concurrent
	 * vm_map_anon on another thread of this task can never pick an
	 * overlapping base_va. */
	space->mmap_next_va = base_va + span;

	nxu_spin_unlock(&space->lock);

	uint64_t *pages = kmalloc(page_count * sizeof(uint64_t));
	if (pages == 0) return false;

	uint64_t allocated = 0ULL;
	for (; allocated < page_count; allocated++) {
		if (!pmm_allocate_page(&pages[allocated])) break;
	}

	if (allocated != page_count) {
		for (uint64_t index = 0ULL; index < allocated; index++) {
			(void)pmm_free_page(pages[index]);
		}
		(void)kfree(pages);
		return false;
	}

	uint64_t mapped = 0ULL;
	for (; mapped < page_count; mapped++) {
		uint64_t page_va = base_va + mapped * PMM_PAGE_SIZE;
		if (!vm_address_space_map_page(space, page_va, pages[mapped], protection)) break;
	}

	if (mapped != page_count) {
		for (uint64_t index = 0ULL; index < mapped; index++) {
			uint64_t page_va = base_va + index * PMM_PAGE_SIZE;
			(void)vm_address_space_unmap_page(space, page_va);
		}
		for (uint64_t index = 0ULL; index < page_count; index++) {
			(void)pmm_free_page(pages[index]);
		}
		(void)kfree(pages);
		return false;
	}

	vm_map_entry_t entry = kmalloc(sizeof(struct vm_map_entry));
	if (entry == 0) {
		/* Mapping succeeded but bookkeeping didn't -- undo it rather than
		 * leave behind a mapping vm_map_free/destroy_all could never find. */
		for (uint64_t index = 0ULL; index < page_count; index++) {
			uint64_t page_va = base_va + index * PMM_PAGE_SIZE;
			(void)vm_address_space_unmap_page(space, page_va);
			(void)pmm_free_page(pages[index]);
		}
		(void)kfree(pages);
		return false;
	}

	(void)kfree(pages);

	entry->next = 0;
	entry->start_va = base_va;
	entry->end_va = base_va + span;
	entry->protection = protection;

	nxu_spin_lock(&space->lock);

	vm_map_entry_t *link = &space->mmap_entries;
	while (*link != 0) link = &(*link)->next;
	*link = entry;

	nxu_spin_unlock(&space->lock);

	*out_va = base_va;
	return true;
}

bool vm_map_free(
	vm_address_space_t *space,
	uint64_t va,
	uint64_t size_bytes
)
{
	if (space == 0 || size_bytes == 0ULL) return false;

	uint64_t page_count = (size_bytes + PMM_PAGE_SIZE - 1ULL) / PMM_PAGE_SIZE;
	uint64_t span = page_count * PMM_PAGE_SIZE;
	uint64_t end_va = va + span;

	nxu_spin_lock(&space->lock);

	vm_map_entry_t *link = &space->mmap_entries;
	vm_map_entry_t entry = 0;
	while (*link != 0) {
		if ((*link)->start_va <= va && end_va <= (*link)->end_va) {
			entry = *link;
			break;
		}
		link = &(*link)->next;
	}

	if (entry == 0) {
		/* Not fully covered by exactly one tracked region -- reject rather
		 * than guess at splitting work across more than one region. */
		nxu_spin_unlock(&space->lock);
		return false;
	}

	bool whole = va == entry->start_va && end_va == entry->end_va;
	bool prefix = va == entry->start_va && end_va < entry->end_va;
	bool suffix = va > entry->start_va && end_va == entry->end_va;
	bool middle = !whole && !prefix && !suffix;

	vm_map_entry_t new_entry = 0;
	if (middle) {
		new_entry = kmalloc(sizeof(struct vm_map_entry));
		if (new_entry == 0) {
			nxu_spin_unlock(&space->lock);
			return false;
		}
	}

	if (whole) {
		*link = entry->next;
	} else if (prefix) {
		entry->start_va = end_va;
	} else if (suffix) {
		entry->end_va = va;
	} else {
		new_entry->next = entry->next;
		new_entry->start_va = end_va;
		new_entry->end_va = entry->end_va;
		new_entry->protection = entry->protection;
		entry->end_va = va;
		entry->next = new_entry;
	}

	nxu_spin_unlock(&space->lock);

	for (uint64_t offset = 0ULL; offset < span; offset += PMM_PAGE_SIZE) {
		uint64_t page_va = va + offset;
		vm_user_page_mapping_t mapping;
		if (vm_address_space_query_page(space, page_va, &mapping)) {
			(void)vm_address_space_unmap_page(space, page_va);
			(void)pmm_free_page(mapping.physical_address);
		}
	}

	if (whole) (void)kfree(entry);

	return true;
}

bool vm_map_lookup(
	const vm_address_space_t *space,
	uint64_t va,
	uint64_t *out_start_va,
	uint64_t *out_end_va
)
{
	if (space == 0) return false;

	/* space is logically const to callers (this only inspects the region
	 * list); the lock still has to be taken since vm_map_anon/vm_map_free
	 * mutate that same list from other threads of the same task. */
	nxu_spinlock_t *lock = (nxu_spinlock_t *)&space->lock;

	nxu_spin_lock(lock);

	bool found = false;
	for (vm_map_entry_t entry = space->mmap_entries; entry != 0; entry = entry->next) {
		if (va >= entry->start_va && va < entry->end_va) {
			if (out_start_va != 0) *out_start_va = entry->start_va;
			if (out_end_va != 0) *out_end_va = entry->end_va;
			found = true;
			break;
		}
	}

	nxu_spin_unlock(lock);

	return found;
}

void vm_map_destroy_all(vm_address_space_t *space)
{
	if (space == 0) return;

	vm_map_entry_t entry = space->mmap_entries;
	space->mmap_entries = 0;
	space->mmap_next_va = 0ULL;

	while (entry != 0) {
		vm_map_entry_t next = entry->next;

		for (uint64_t page_va = entry->start_va; page_va < entry->end_va; page_va += PMM_PAGE_SIZE) {
			vm_user_page_mapping_t mapping;
			if (vm_address_space_query_page(space, page_va, &mapping)) {
				(void)vm_address_space_unmap_page(space, page_va);
				(void)pmm_free_page(mapping.physical_address);
			}
		}

		(void)kfree(entry);
		entry = next;
	}
}
