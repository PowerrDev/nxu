/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/vm_fault.c
 *
 * See vm/vm_fault.h.
 */

#include <vm/vm_fault.h>

#include <mach/machine/vm_param.h>
#include <vm/pmm.h>
#include <vm/vm_map.h>

#include <stdbool.h>
#include <stdint.h>

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
		/* Present: the only fault worth resolving is a write to a page
		 * fork left shared. A read or fetch of a mapped page that still
		 * faulted is a permission violation. */
		if (access == VM_FAULT_WRITE && mapping.cow) {
			return vm_address_space_cow_break(space, page_va);
		}

		return false;
	}

	vm_user_protection_t protection;

	if (!vm_map_region_at(space, page_va, &protection, 0, 0)) return false;

	if (access == VM_FAULT_WRITE && protection != VM_USER_PROTECTION_READ_WRITE) return false;
	if (access == VM_FAULT_EXECUTE && protection != VM_USER_PROTECTION_READ_EXECUTE) return false;

	/* The allocator hands back zeroed pages, which is exactly what an
	 * untouched anonymous page must contain. */
	uint64_t physical;

	if (!pmm_allocate_page(&physical)) return false;

	if (!vm_address_space_map_page(space, page_va, physical, protection)) {
		(void)pmm_free_page(physical);
		return false;
	}

	return true;
}
