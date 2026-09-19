/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/i386/devices_shim.c
 *
 * Standalone fallbacks so the devices area builds and boots before its
 * neighbours have merged. Every definition here is weak: as soon as the VM
 * area supplies vm/pmm.c and vm/vmm.c, or the interrupts area supplies
 * kern/irq/irq.c, their strong symbols replace these and this file does
 * nothing.
 *
 *   pmm_* / vmm_physical_to_higher_half
 *       A small page pool carved out of the kernel image (bss) for the
 *       virtqueues and DMA bounce pages the drivers allocate. It is
 *       physically contiguous and page-aligned. A page's physical address is
 *       its link address, less VMM_HIGHER_HALF_BASE when the image is linked
 *       in the higher half, so the same code is right with paging off (linked
 *       at 1 MiB, virtual == physical) and on.
 *
 *   irq_register / irq_unregister
 *       Refuse every request, so drivers stay in polling mode.
 *
 * The drivers reach memory only through the same pmm_allocate_page() /
 * vmm_physical_to_higher_half() calls the arm64 build uses.
 */

#include <kern/irq/irq.h>
#include <mach/machine/vm_param.h>
#include <vm/pmm.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>

#define SHIM_POOL_PAGES 64U

#define SHIM_WEAK __attribute__((weak))

static uint8_t g_shim_pool[SHIM_POOL_PAGES * 4096U] __attribute__((aligned(4096)));
static bool g_shim_used[SHIM_POOL_PAGES];

static uint64_t shim_link_offset(void)
{
	return (uintptr_t)g_shim_pool >= VMM_HIGHER_HALF_BASE ? (uint64_t)VMM_HIGHER_HALF_BASE : 0ULL;
}

static uint64_t shim_pool_physical(void)
{
	return (uint64_t)(uintptr_t)g_shim_pool - shim_link_offset();
}

SHIM_WEAK bool pmm_allocate_contiguous_pages(uint64_t page_count, uint64_t *physical_address)
{
	if (physical_address == 0 || page_count == 0ULL || page_count > SHIM_POOL_PAGES) return false;

	uint32_t run = 0U;

	for (uint32_t index = 0U; index < SHIM_POOL_PAGES; index++) {
		run = g_shim_used[index] ? 0U : run + 1U;

		if (run == page_count) {
			uint32_t first = index + 1U - (uint32_t)page_count;

			for (uint32_t page = first; page <= index; page++) g_shim_used[page] = true;

			*physical_address = shim_pool_physical() + (uint64_t)first * PMM_PAGE_SIZE;
			return true;
		}
	}

	return false;
}

SHIM_WEAK bool pmm_free_contiguous_pages(uint64_t physical_address, uint64_t page_count)
{
	uint64_t base = shim_pool_physical();

	if (physical_address < base || (physical_address - base) % PMM_PAGE_SIZE != 0ULL) return false;

	uint64_t first = (physical_address - base) / PMM_PAGE_SIZE;

	if (page_count == 0ULL || first + page_count > SHIM_POOL_PAGES) return false;

	for (uint64_t page = first; page < first + page_count; page++) {
		if (!g_shim_used[page]) return false;
	}

	for (uint64_t page = first; page < first + page_count; page++) g_shim_used[page] = false;

	return true;
}

SHIM_WEAK bool pmm_allocate_page(uint64_t *physical_address)
{
	return pmm_allocate_contiguous_pages(1ULL, physical_address);
}

SHIM_WEAK bool pmm_free_page(uint64_t physical_address)
{
	return pmm_free_contiguous_pages(physical_address, 1ULL);
}

SHIM_WEAK bool vmm_higher_half_direct_map_enabled(void)
{
	return true;
}

SHIM_WEAK bool vmm_physical_to_higher_half(uint64_t physical_address, uint64_t *virtual_address)
{
	if (virtual_address == 0 || physical_address >= 0x100000000ULL) return false;

	*virtual_address = physical_address + shim_link_offset();
	return true;
}

SHIM_WEAK bool irq_register(uint32_t intid, irq_handler_t handler, void *context)
{
	(void)intid;
	(void)handler;
	(void)context;
	return false;
}

SHIM_WEAK bool irq_unregister(uint32_t intid, irq_handler_t handler, void *context)
{
	(void)intid;
	(void)handler;
	(void)context;
	return false;
}
