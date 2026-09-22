/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/vm_init.c
 *
 * The "vm" boot phase: physical memory, kernel page tables, the kernel
 * virtual-address arena. See doc/i386/vm.md for the whole sequence.
 *
 * What is already true on entry: start.S enabled paging and the higher-half
 * kernel, so the kernel runs at 0xC01xxxxx and all of RAM below 768 MiB is
 * reachable through the direct map; i386_boot_relocate() moved the Multiboot
 * structure into that map and removed the temporary identity mapping.
 *
 * The phase deliberately builds its own platform_t from the Multiboot memory
 * map instead of reading whatever the platform phase left behind, so it works
 * (and can be booted with test=vm) on its own.
 */

#include <kern/i386/boot_info.h>

#include <kern/i386/memory_map.h>
#include <kern/i386/pmap.h>

#include <kern/console/console.h>
#include <vm/pmm.h>
#include <vm/vm_kern.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

static void vm_init_reserve_boot_range(uint64_t base, uint64_t size, void *context)
{
	uint32_t *count = (uint32_t *)context;

	if (pmm_reserve_boot_range(base, size)) {
		kprintf("vm_init_reserve_boot_range: keeping 0x%llx-0x%llx out of the allocator\n", base, base + size);
		(*count)++;
	} else {
		kprintf("vm_init_reserve_boot_range: cannot reserve 0x%llx+0x%llx\n", base, size);
	}
}

bool i386_init_vm(const i386_boot_info_t *boot)
{
	platform_t platform;
	uint32_t reserved = 0U;

	memset(&platform, 0, sizeof(platform));

	if (boot == 0 || boot->multiboot == 0) {
		kputln("i386_init_vm: no Multiboot information, cannot size memory");
		return false;
	}

	if (!i386_memory_map_load(&platform, boot->multiboot)) {
		kputln("i386_init_vm: no usable RAM in the memory map");
		return false;
	}

	i386_memory_map_dump(&platform);

	/* Keep the loader's data (command line above all) out of the allocator. */
	(void)i386_memory_map_boot_ranges(boot->multiboot, vm_init_reserve_boot_range, &reserved);

	/*
	 * SMP_TRAMPOLINE_PHYS (kern/i386/smp.c/smp_trampoline.S): a page-aligned,
	 * sub-1-MiB physical page a secondary CPU's real-mode SIPI vector must be
	 * able to address, and where smp_boot_secondaries() copies the trampoline
	 * and writes its parameter block before starting anything. Reserved here,
	 * before the allocator ever hands out a single page, whether or not this
	 * boot ever actually brings up a second CPU: the address must stay free
	 * the whole time regardless, and reserving it after the fact would race
	 * whatever else had already claimed it.
	 */
	vm_init_reserve_boot_range(0x8000ULL, 0x1000ULL, &reserved);

	if (!pmm_init(&platform, 0)) {
		kputln("i386_init_vm: pmm_init failed");
		return false;
	}

	kprintf(
		"i386_init_vm: pmm %llu page(s), %llu free, %u boot range(s) reserved, kernel 0x%x-0x%x\n",
		pmm_get_page_count(),
		pmm_get_free_page_count(),
		reserved,
		(uint32_t)(uintptr_t)__kernel_start,
		(uint32_t)(uintptr_t)__kernel_end
	);

	if (!vmm_init(&platform)) {
		kputln("i386_init_vm: vmm_init failed");
		return false;
	}

	if (!vmm_validate_linked_kernel_layout()) {
		kputln("i386_init_vm: kernel layout validation failed");
		return false;
	}

	if (!vmm_map_higher_half_direct_map(&platform)) {
		kputln("i386_init_vm: direct map validation failed");
		return false;
	}

	if (!vmm_validate_kernel_permissions()) {
		kputln("i386_init_vm: kernel permission validation failed");
		return false;
	}

	if (!vm_kern_init()) {
		kputln("i386_init_vm: vm_kern_init failed");
		return false;
	}

	kprintf(
		"i386_init_vm: paging on, cr3 0x%x, %llu free page(s), vm_kern arena 0x%x-0x%x, mmio window 0x%x-0x%x\n",
		pmap_current_directory_physical(),
		pmm_get_free_page_count(),
		(uint32_t)VM_KERN_BASE,
		(uint32_t)(VM_KERN_END - 1ULL),
		(uint32_t)VM_MMIO_BASE,
		(uint32_t)(VM_MMIO_BASE + VM_MMIO_SIZE - 1U)
	);

	return true;
}
