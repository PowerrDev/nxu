/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/x86_64/vm_init.c
 *
 * Coordinates the x86-64 physical allocator, kernel VMM, and kernel virtual
 * allocation arena after the bootstrap root is active.
 */

#include <vm/x86_64/bootstrap.h>

#include <platform/x86_64/memory_map.h>
#include <kern/console/console.h>
#include <kern/memory/heap.h>
#include <vm/pmm.h>
#include <vm/vm_kern.h>
#include <vm/vmm.h>

#include <stdint.h>

#define X86_VM_HANDOFF_ALLOCATION_SIZE (32ULL * 4096ULL)
#define X86_VM_HANDOFF_OFFSET 256ULL

/*
 * Routine:     x86_vm_initialize
 * Purpose:
 *              Parse usable UEFI RAM, reserve loader handoff pages, initialize
 *              the physical allocator and VMM, then ready the kernel arena.
 */
bool
x86_vm_initialize(const struct nxu_boot_x86_64 *boot)
{
	platform_t platform;

	if (boot != 0) {
		kprintf("x86_vm_initialize: map 0x%llx, bytes %llu, stride %llu\n",
				(unsigned long long)boot->memory_map, (unsigned long long)boot->memory_map_size,
				(unsigned long long)boot->memory_descriptor_size);
	}

	if (!x86_platform_memory_map_load(boot, &platform)) {
		kprintf("x86_vm_initialize: UEFI RAM map rejected\n");
		return false;
	}

	kprintf("x86_vm_initialize: %u usable RAM range(s)\n", platform.memory_region_count);
	uint64_t usable_bytes = 0ULL;
	for (uint32_t index = 0U; index < platform.memory_region_count; index++) {
		const platform_region_t *region = &platform.memory_regions[index];
		usable_bytes += region->size;
		kprintf("x86_vm_initialize: RAM[%u] 0x%llx-0x%llx, %llu MiB\n", index,
				(unsigned long long)region->base, (unsigned long long)(region->base + region->size),
				(unsigned long long)(region->size / (1024ULL * 1024ULL)));
	}
	kprintf("x86_vm_initialize: %llu MiB usable below the 1 GiB direct-map limit\n",
			(unsigned long long)(usable_bytes / (1024ULL * 1024ULL)));

	uint64_t boot_address = (uint64_t)(uintptr_t)boot;
	if (boot_address < X86_VM_HANDOFF_OFFSET ||
		!pmm_reserve_boot_range(boot_address - X86_VM_HANDOFF_OFFSET, X86_VM_HANDOFF_ALLOCATION_SIZE)) {
		kprintf("x86_vm_initialize: boot handoff reservation failed\n");
		return false;
	}

	if (!pmm_init(&platform, 0)) {
		kprintf("x86_vm_initialize: physical allocator initialization failed\n");
		return false;
	}

	kprintf("x86_vm_initialize: pmm has %llu free page(s)\n", pmm_get_free_page_count());
	pmm_dump();

	if (!vmm_init(&platform)) {
		kprintf("x86_vm_initialize: VMM root initialization failed\n");
		return false;
	}
    
	vmm_dump();
	vmm_dump_higher_half();

	if (!vmm_validate_linked_kernel_layout() || !vmm_validate_higher_half_direct_map(&platform) ||
		!vmm_validate_kernel_permissions()) {
		kprintf("x86_vm_initialize: VMM validation failed\n");
		return false;
	}

	if (!vm_kern_init()) {
		kprintf("x86_vm_initialize: kernel arena initialization failed\n");
		return false;
	}
	vm_kern_dump();

	if (!heap_init()) {
		kprintf("x86_vm_initialize: kernel heap initialization failed\n");
		return false;
	}
	heap_dump();

	return true;
}
