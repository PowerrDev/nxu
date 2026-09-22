/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/vm_fault.c
 *
 * See vm/vm_fault.h.
 *
 * SMP. Threads of one process fault on different CPUs at the same time, on
 * different pages and on the same one, while other threads mmap, munmap and fork.
 * The rules that make that correct:
 *
 *   - A fault is a hint that something was wrong when the instruction ran, not a
 *     fact about the tables now. By the time this code looks, another CPU may
 *     already have fixed it. "The page is present and allows this access" is
 *     therefore success (the instruction can be retried), never a violation.
 *   - The region check and the page install are one critical section under
 *     space->lock, so an munmap that removes the region cannot be followed by a
 *     fault that maps a page into it.
 *   - The page is allocated before the lock is taken (the allocator has its own
 *     lock and must not be taken under this one for longer than it has to be). If
 *     the install loses the race the page is given back. Exactly one mapping
 *     ever ends up in the slot.
 */

#include <vm/vm_fault.h>

#include <kern/machine/vm_param.h>
#include <kern/lock.h>
#include <vm/pmm.h>
#include <vm/vm_map.h>

#include <stdbool.h>
#include <stdint.h>

static bool vm_fault_access_satisfied(vm_fault_access_t access, vm_user_protection_t protection)
{
	switch (access) {
	case VM_FAULT_WRITE: return protection == VM_USER_PROTECTION_READ_WRITE;
	case VM_FAULT_EXECUTE: return protection == VM_USER_PROTECTION_READ_EXECUTE;
	default: return true;
	}
}

/* The page is mapped: resolve a write to a COW page, or accept a fault another CPU already fixed. */
static bool vm_fault_present(
	vm_address_space_t *space,
	uint64_t page_va,
	vm_fault_access_t access,
	const vm_user_page_mapping_t *mapping
)
{
	if (access == VM_FAULT_WRITE && mapping->cow) {
		return vm_address_space_cow_break(space, page_va);
	}

	return vm_fault_access_satisfied(access, mapping->protection);
}

bool vm_fault_user(
	vm_address_space_t *space,
	uint64_t virtual_address,
	vm_fault_access_t access
)
{
	if (
		space == 0 ||
		virtual_address < PMM_PAGE_SIZE ||
		virtual_address >= VM_MAX_USER_ADDRESS
	) {
		return false;
	}

	uint64_t page_va = virtual_address & ~(PMM_PAGE_SIZE - 1ULL);
	vm_user_page_mapping_t mapping;

	if (vm_address_space_query_page(space, page_va, &mapping)) {
		return vm_fault_present(space, page_va, access, &mapping);
	}

	vm_user_protection_t protection;

	if (!vm_map_region_at(space, page_va, &protection, 0, 0)) return false;

	if (access == VM_FAULT_WRITE && protection != VM_USER_PROTECTION_READ_WRITE) return false;
	if (access == VM_FAULT_EXECUTE && protection != VM_USER_PROTECTION_READ_EXECUTE) return false;

	/* The allocator hands back zeroed pages, which is exactly what an
	 * untouched anonymous page must contain. */
	uint64_t physical;

	if (!pmm_allocate_page(&physical)) return false;

	nxu_spin_lock(&space->lock);

	vm_user_protection_t current;
	bool in_region = vm_map_region_at_locked(space, page_va, &current);
	bool installed = in_region && vm_address_space_map_page_locked(space, page_va, physical, current);

	nxu_spin_unlock(&space->lock);

	if (installed) return true;

	(void)pmm_free_page(physical);

	/* Unmapped since the first look: a genuine fault. */
	if (!in_region) return false;

	/* The slot was taken: another CPU resolved this same fault first. */
	if (vm_address_space_query_page(space, page_va, &mapping)) {
		return vm_fault_present(space, page_va, access, &mapping);
	}

	return false;
}
