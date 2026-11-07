/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/vm_kmap.c
 *
 * The one neutral translation unit permitted to include <vm/vmm.h>. It binds
 * the architecture-neutral kernel-map facade (vm/vm_kmap.h) to the port's VMM,
 * keeping the arm64-flavoured VMM names (higher-half / TTBR) out of the shared
 * VM core.
 */
#include <vm/vm_kmap.h>
#include <vm/vmm.h>

/*
 * The facade enums are laid out to match the VMM's, so each forward is a cast
 * rather than a switch. These assertions make that contract explicit and fail
 * the build if either enumeration is ever reordered.
 */
_Static_assert((int)VM_KMAP_PROT_READ_WRITE == (int)VMM_PROTECTION_READ_WRITE, "kmap/vmm RW protection diverged");
_Static_assert((int)VM_KMAP_PROT_READ_ONLY == (int)VMM_PROTECTION_READ_ONLY, "kmap/vmm RO protection diverged");
_Static_assert((int)VM_KMAP_PROT_READ_EXECUTE == (int)VMM_PROTECTION_READ_EXECUTE, "kmap/vmm RX protection diverged");
_Static_assert((int)VM_KMAP_MEM_NORMAL == (int)VMM_MEMORY_NORMAL, "kmap/vmm normal memory diverged");
_Static_assert((int)VM_KMAP_MEM_DEVICE == (int)VMM_MEMORY_DEVICE, "kmap/vmm device memory diverged");

bool
vm_kmap_enabled(void)
{
	return vmm_is_enabled();
}

bool
vm_kmap_map_page(uint64_t virtual_address, uint64_t physical_address, vm_kmap_mem_t memory_type,
				 vm_kmap_prot_t protection)
{
	return vmm_map_page(virtual_address, physical_address, (vmm_memory_type_t)memory_type,
						(vmm_protection_t)protection);
}

bool
vm_kmap_unmap_page(uint64_t virtual_address, uint64_t *physical_address)
{
	return vmm_unmap_page(virtual_address, physical_address);
}

bool
vm_kmap_query_page(uint64_t virtual_address, vm_kmap_mapping_t *mapping)
{
	if (mapping == 0) {
		return false;
	}

	vmm_page_mapping_t native;

	if (!vmm_query_page(virtual_address, &native)) {
		return false;
	}

	mapping->physical_address = native.physical_address;
	mapping->memory_type = (vm_kmap_mem_t)native.memory_type;
	mapping->protection = (vm_kmap_prot_t)native.protection;

	return true;
}

bool
vm_kmap_direct_map_enabled(void)
{
	return vmm_higher_half_direct_map_enabled();
}

bool
vm_kmap_phys_to_direct(uint64_t physical_address, uint64_t *virtual_address)
{
	return vmm_physical_to_higher_half(physical_address, virtual_address);
}

bool
vm_kmap_direct_to_phys(uint64_t virtual_address, uint64_t *physical_address)
{
	return vmm_higher_half_to_physical(virtual_address, physical_address);
}

bool
vm_kmap_address_to_phys(uint64_t kernel_address, uint64_t *physical_address)
{
	return vmm_kernel_address_to_physical(kernel_address, physical_address);
}
