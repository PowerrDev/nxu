/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/vmm_i386.c
 *
 * i386 implementation of vm/vmm.h: the kernel's own translations, on top of
 * pmap.c. The arm64 implementation (vm/vmm*.c) is not built for i386.
 *
 * What the arm64 names mean here:
 *
 *   TTBR1 / higher-half     the kernel half of every page directory
 *                           (virtual 0xC0000000 and up), shared by all
 *                           address spaces;
 *   direct map              physical [0, VM_DIRECT_MAP_SIZE) at
 *                           VMM_HIGHER_HALF_BASE + physical, made of 4 MiB
 *                           pages by start.S and there from the first
 *                           instruction of C, so the *_higher_half helpers
 *                           are valid before vmm_init();
 *   TTBR0                   the user half, owned by address_space_i386.c;
 *   memory type             VMM_MEMORY_DEVICE maps uncached (PCD|PWT);
 *   READ_EXECUTE            no NX exists: kernel pages are always executable,
 *                           so this is a read-only page tagged with a
 *                           software bit for round-tripping through queries.
 *
 * Live mapping (vmm_map_page and friends) is restricted to the vm_kern arena
 * and the MMIO window, the parts of the kernel half backed by pre-allocated
 * page tables; the direct map and the kernel image are not remappable.
 */

#include <vm/vmm.h>

#include <kern/i386/pmap.h>

#include <kern/console/console.h>
#include <vm/address_space.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];
extern uint8_t __text_start[];
extern uint8_t __text_end[];
extern uint8_t __rodata_start[];
extern uint8_t __rodata_end[];
extern uint8_t __data_start[];
extern uint8_t __data_end[];
extern uint8_t __bss_start[];
extern uint8_t __bss_end[];
extern uint8_t i386_boot_stack_guard[];

/* First byte past the top of the 32-bit address space. */
#define VMM_ADDRESS_LIMIT 0x100000000ULL

/* The image starts at 1 MiB physical, as the linker script and start.S assume. */
#define VMM_KERNEL_PHYSICAL_BASE 0x00100000ULL

static bool g_vmm_enabled;

static bool vmm_entry_flags(vmm_memory_type_t memory_type, vmm_protection_t protection, uint32_t *flags)
{
	uint32_t value = PTE_PRESENT;

	switch (protection) {
	case VMM_PROTECTION_READ_WRITE:
		value |= PTE_WRITE;
		break;

	case VMM_PROTECTION_READ_ONLY:
		break;

	case VMM_PROTECTION_READ_EXECUTE:
		value |= PTE_SOFTWARE_EXEC;
		break;

	default:
		return false;
	}

	switch (memory_type) {
	case VMM_MEMORY_NORMAL:
		break;

	case VMM_MEMORY_DEVICE:
		value |= PTE_CACHE_DISABLE | PTE_WRITE_THROUGH;
		break;

	default:
		return false;
	}

	*flags = value;
	return true;
}

static vmm_protection_t vmm_entry_protection(uint32_t entry)
{
	if ((entry & PTE_WRITE) != 0U) return VMM_PROTECTION_READ_WRITE;
	return (entry & PTE_SOFTWARE_EXEC) != 0U ? VMM_PROTECTION_READ_EXECUTE : VMM_PROTECTION_READ_ONLY;
}

static bool vmm_page_aligned(uint64_t value)
{
	return (value & (uint64_t)PMAP_PAGE_MASK) == 0ULL;
}

/* Addresses vmm_map_page/vmm_unmap_page may touch: arena, fixmap, MMIO window. */
static bool vmm_live_range(uint64_t virtual_address)
{
	return virtual_address >= VM_KERN_BASE && virtual_address < VMM_ADDRESS_LIMIT;
}

/*
 * Resolve a kernel-half address to its leaf entry value and the physical
 * address it maps (with the page offset), whether the leaf is a 4 KiB page or
 * a 4 MiB direct-map page. `writable` reports the effective write permission
 * for supervisor accesses (both levels must allow it).
 */
static bool vmm_walk(uint64_t virtual_address, uint32_t *leaf, uint64_t *physical_address, bool *writable)
{
	if (!g_vmm_enabled || virtual_address < VMM_HIGHER_HALF_BASE || virtual_address >= VMM_ADDRESS_LIMIT) return false;

	uint32_t address = (uint32_t)virtual_address;
	uint32_t directory_entry = pmap_kernel_directory()[pmap_directory_index(address)];

	if ((directory_entry & PTE_PRESENT) == 0U) return false;

	if ((directory_entry & PTE_LARGE) != 0U) {
		if (leaf != 0) *leaf = directory_entry;
		if (physical_address != 0) *physical_address = (uint64_t)(directory_entry & 0xFFC00000U) | (address & (PMAP_LARGE_PAGE_SIZE - 1U));
		if (writable != 0) *writable = (directory_entry & PTE_WRITE) != 0U;
		return true;
	}

	uint32_t entry = pmap_table_pointer(directory_entry & PTE_FRAME)[pmap_table_index(address)];

	if ((entry & PTE_PRESENT) == 0U) return false;

	if (leaf != 0) *leaf = entry;
	if (physical_address != 0) *physical_address = (uint64_t)(entry & PTE_FRAME) | (address & PMAP_PAGE_MASK);
	if (writable != 0) *writable = (entry & PTE_WRITE) != 0U && (directory_entry & PTE_WRITE) != 0U;
	return true;
}

bool vmm_init(const platform_t *platform)
{
	if (g_vmm_enabled || platform == 0 || platform->memory_region_count == 0U) return false;

	for (uint32_t index = 0U; index < platform->memory_region_count; index++) {
		const platform_region_t *region = &platform->memory_regions[index];

		if (region->size == 0ULL || region->base + region->size > VM_DIRECT_MAP_SIZE) {
			kprintf("vmm_init: region %u is not covered by the direct map\n", index);
			return false;
		}
	}

	if (!pmap_init()) return false;

	g_vmm_enabled = true;
	return true;
}

/* Paging is on from the first instruction of C; there is no identity mapping to add. */
bool vmm_map_identity(
	uint64_t physical_address,
	uint64_t size,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection
)
{
	(void)physical_address;
	(void)size;
	(void)memory_type;
	(void)protection;
	return false;
}

bool vmm_map_page(
	uint64_t virtual_address,
	uint64_t physical_address,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection
)
{
	uint32_t flags;

	if (
		!g_vmm_enabled ||
		!vmm_page_aligned(virtual_address) ||
		!vmm_page_aligned(physical_address) ||
		!vmm_live_range(virtual_address) ||
		physical_address >= VMM_ADDRESS_LIMIT ||
		!vmm_entry_flags(memory_type, protection, &flags)
	) {
		return false;
	}

	uint32_t *entry;

	if (!pmap_get_entry(pmap_kernel_directory(), (uint32_t)virtual_address, false, false, 0, &entry)) return false;
	if ((*entry & PTE_PRESENT) != 0U) return false;

	*entry = (uint32_t)physical_address | flags;
	pmap_invalidate_page((uint32_t)virtual_address);
	return true;
}

bool vmm_unmap_page(uint64_t virtual_address, uint64_t *physical_address)
{
	if (!g_vmm_enabled || !vmm_page_aligned(virtual_address) || !vmm_live_range(virtual_address)) return false;

	uint32_t *entry;

	if (!pmap_get_entry(pmap_kernel_directory(), (uint32_t)virtual_address, false, false, 0, &entry)) return false;
	if ((*entry & PTE_PRESENT) == 0U) return false;

	if (physical_address != 0) *physical_address = *entry & PTE_FRAME;

	*entry = 0U;
	pmap_invalidate_page((uint32_t)virtual_address);
	return true;
}

bool vmm_protect_page(uint64_t virtual_address, vmm_protection_t protection)
{
	if (
		!g_vmm_enabled ||
		!vmm_page_aligned(virtual_address) ||
		virtual_address < VMM_HIGHER_HALF_BASE ||
		virtual_address >= VMM_ADDRESS_LIMIT
	) {
		return false;
	}

	uint32_t flags;

	if (!vmm_entry_flags(VMM_MEMORY_NORMAL, protection, &flags)) return false;

	uint32_t *entry;

	if (!pmap_get_entry(pmap_kernel_directory(), (uint32_t)virtual_address, false, false, 0, &entry)) return false;
	if ((*entry & PTE_PRESENT) == 0U) return false;

	*entry = (*entry & ~(PTE_WRITE | PTE_SOFTWARE_EXEC)) | (flags & (PTE_WRITE | PTE_SOFTWARE_EXEC));
	pmap_invalidate_page((uint32_t)virtual_address);
	return true;
}

bool vmm_query_page(uint64_t virtual_address, vmm_page_mapping_t *mapping)
{
	uint32_t leaf;
	uint64_t physical;

	if (mapping == 0 || !vmm_walk(virtual_address & ~(uint64_t)PMAP_PAGE_MASK, &leaf, &physical, 0)) return false;

	mapping->physical_address = physical;
	mapping->memory_type = (leaf & PTE_CACHE_DISABLE) != 0U ? VMM_MEMORY_DEVICE : VMM_MEMORY_NORMAL;
	mapping->protection = vmm_entry_protection(leaf);
	return true;
}

bool vmm_translate(uint64_t virtual_address, uint64_t *physical_address)
{
	return physical_address != 0 && vmm_walk(virtual_address, 0, physical_address, 0);
}

bool vmm_translate_write(uint64_t virtual_address, uint64_t *physical_address)
{
	bool writable;

	return physical_address != 0 && vmm_walk(virtual_address, 0, physical_address, &writable) && writable;
}

bool vmm_is_enabled(void)
{
	return g_vmm_enabled;
}

bool vmm_is_higher_half_address(uint64_t virtual_address)
{
	return virtual_address >= VMM_HIGHER_HALF_BASE && virtual_address < VMM_ADDRESS_LIMIT;
}

bool vmm_higher_half_enabled(void)
{
	return true;
}

bool vmm_higher_half_direct_map_enabled(void)
{
	return true;
}

bool vmm_init_higher_half_alias(void)
{
	/* start.S installed the kernel alias before the first C instruction. */
	return true;
}

bool vmm_physical_to_higher_half(uint64_t physical_address, uint64_t *virtual_address)
{
	if (virtual_address == 0 || physical_address >= VM_DIRECT_MAP_SIZE) return false;

	*virtual_address = physical_address + VMM_HIGHER_HALF_BASE;
	return true;
}

bool vmm_higher_half_to_physical(uint64_t virtual_address, uint64_t *physical_address)
{
	if (
		physical_address == 0 ||
		virtual_address < VMM_HIGHER_HALF_BASE ||
		virtual_address >= (uint64_t)VMM_HIGHER_HALF_BASE + VM_DIRECT_MAP_SIZE
	) {
		return false;
	}

	*physical_address = virtual_address - VMM_HIGHER_HALF_BASE;
	return true;
}

bool vmm_kernel_address_to_physical(uint64_t kernel_address, uint64_t *physical_address)
{
	if (physical_address == 0) return false;

	/* The direct map covers the kernel image, so its symbols are direct-map addresses. */
	if (vmm_higher_half_to_physical(kernel_address, physical_address)) return true;

	/* Above the direct map (arena, MMIO): ask the page tables. */
	if (kernel_address >= (uint64_t)VMM_HIGHER_HALF_BASE + VM_DIRECT_MAP_SIZE) return vmm_translate(kernel_address, physical_address);

	/* A physical address, as the bootstrap alias would produce. */
	if (kernel_address < VM_DIRECT_MAP_SIZE) {
		*physical_address = kernel_address;
		return true;
	}

	return false;
}

bool vmm_map_higher_half_direct_map(const platform_t *platform)
{
	/* The direct map exists already; this just confirms it covers the platform's RAM. */
	return g_vmm_enabled && platform != 0 && vmm_validate_higher_half_direct_map(platform);
}

bool vmm_rebase_kernel_pointers(void)
{
	/* Table pointers are direct-map addresses from the start. */
	return g_vmm_enabled;
}

bool vmm_disable_ttbr0(void)
{
	return g_vmm_enabled && vm_address_space_deactivate();
}

bool vmm_ttbr0_disabled(void)
{
	return vm_address_space_current() == 0;
}

uint64_t vmm_get_ttbr1_root(void)
{
	return pmap_kernel_directory_physical();
}

uint64_t vmm_get_ttbr1_table_count(void)
{
	return (uint64_t)pmap_kernel_table_count() + 1ULL;
}

static bool vmm_validate_section(uint64_t start, uint64_t end, bool writable)
{
	for (uint64_t address = start; address < end; address += PMAP_PAGE_SIZE) {
		uint64_t expected;
		uint64_t physical;

		/* The boot stack's guard page is deliberately unmapped. */
		if (address == (uintptr_t)i386_boot_stack_guard) {
			if (vmm_translate(address, &physical)) return false;
			continue;
		}

		if (!vmm_kernel_address_to_physical(address, &expected)) return false;
		if (!vmm_translate(address, &physical) || physical != expected) return false;
		if (vmm_translate_write(address, &physical) != writable) return false;
	}

	return true;
}

bool vmm_validate_kernel_permissions(void)
{
	if (!g_vmm_enabled) return false;

	return
		vmm_validate_section((uintptr_t)__kernel_start, (uintptr_t)__data_start, false) &&
		vmm_validate_section((uintptr_t)__data_start, (uintptr_t)__kernel_end, true);
}

bool vmm_validate_linked_kernel_layout(void)
{
	uint64_t physical;

	if (!g_vmm_enabled) return false;

	if (
		(uintptr_t)__kernel_start != VMM_HIGHER_HALF_BASE + VMM_KERNEL_PHYSICAL_BASE ||
		!vmm_kernel_address_to_physical((uintptr_t)__kernel_start, &physical) ||
		physical != VMM_KERNEL_PHYSICAL_BASE
	) {
		return false;
	}

	/* Sections follow one another, page aligned, in the order the permissions assume. */
	const uint8_t *bounds[] = {
		__kernel_start, __text_start, __text_end, __rodata_start, __rodata_end, __data_start, __data_end, __bss_start, __bss_end, __kernel_end
	};

	for (uint32_t index = 0U; index < sizeof(bounds) / sizeof(bounds[0]); index++) {
		if (((uintptr_t)bounds[index] & PMAP_PAGE_MASK) != 0U) return false;
		if (index != 0U && bounds[index] < bounds[index - 1U]) return false;
	}

	return true;
}

bool vmm_validate_higher_half_direct_map(const platform_t *platform)
{
	if (!g_vmm_enabled || platform == 0 || platform->memory_region_count == 0U) return false;

	for (uint32_t index = 0U; index < platform->memory_region_count; index++) {
		const platform_region_t *region = &platform->memory_regions[index];

		if (region->size < PMAP_PAGE_SIZE || region->base + region->size > VM_DIRECT_MAP_SIZE) return false;

		uint64_t probes[2] = { region->base, region->base + region->size - PMAP_PAGE_SIZE };

		for (uint32_t probe = 0U; probe < 2U; probe++) {
			uint64_t virtual_address;
			uint64_t physical;

			if (!vmm_physical_to_higher_half(probes[probe], &virtual_address)) return false;
			if (!vmm_walk(virtual_address, 0, &physical, 0) || physical != probes[probe]) return false;
		}
	}

	return true;
}

void vmm_dump(void)
{
	kprintf(
		"vmm: %s, kernel directory phys 0x%x, %u kernel table page(s)\n",
		g_vmm_enabled ? "enabled" : "not initialised",
		pmap_kernel_directory_physical(),
		pmap_kernel_table_count()
	);
	kprintf(
		"vmm: image 0x%x-0x%x, text 0x%x-0x%x, rodata 0x%x-0x%x, data 0x%x-0x%x, bss 0x%x-0x%x\n",
		(uint32_t)(uintptr_t)__kernel_start,
		(uint32_t)(uintptr_t)__kernel_end,
		(uint32_t)(uintptr_t)__text_start,
		(uint32_t)(uintptr_t)__text_end,
		(uint32_t)(uintptr_t)__rodata_start,
		(uint32_t)(uintptr_t)__rodata_end,
		(uint32_t)(uintptr_t)__data_start,
		(uint32_t)(uintptr_t)__data_end,
		(uint32_t)(uintptr_t)__bss_start,
		(uint32_t)(uintptr_t)__bss_end
	);
	pmap_dump();
}

void vmm_dump_higher_half(void)
{
	vmm_dump();
}
