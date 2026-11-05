/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/x86_64/vmm.c
 *
 * x86-64 four-level page-table implementation of the shared VMM interface.
 * The bootstrap identity map remains active while the physical direct map,
 * kernel arena, and per-address-space lower halves are added.
 */

#include <vm/x86_64/vmm_internal.h>

#include <kern/console/console.h>
#include <kern/lock.h>
#include <kern/machine/vm_param.h>
#include <platform/x86_64/uart.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

#define X86_CR0_WP (1UL << 16U)
#define X86_CR0_PG (1UL << 31U)
#define X86_CR4_PAE (1UL << 5U)
#define X86_CR4_LA57 (1UL << 12U)
#define X86_MSR_EFER 0xC0000080U
#define X86_EFER_NXE (1ULL << 11U)
#define X86_CPUID_NX (1U << 20U)
#define X86_KERNEL_PML4_INDEX(address) (((address) >> 39U) & X86_VM_INDEX_MASK)

x86_vmm_state_t g_x86_vmm;

static nxu_rlock_t g_x86_vmm_lock = NXU_RLOCK_INIT;

/*
 * Routine:     x86_vmm_read_cr0
 * Purpose:
 *              Read paging and write-protect state while validating the
 *              processor's active translation configuration.
 */
static uintptr_t
x86_vmm_read_cr0(void)
{
	uintptr_t value;

	__asm__ volatile("mov %%cr0, %0" : "=r"(value));

	return value;
}

/*
 * Routine:     x86_vmm_read_cr4
 * Purpose:
 *              Read paging-mode features to reject a mode the four-level
 *              bootstrap does not support.
 */
static uintptr_t
x86_vmm_read_cr4(void)
{
	uintptr_t value;

	__asm__ volatile("mov %%cr4, %0" : "=r"(value));

	return value;
}

/*
 * Routine:     x86_vmm_cpu_has_nx
 * Purpose:
 *              Detect the extended CPUID execute-disable feature before
 *              writing EFER.NXE or using NX page-table bits.
 */
static bool
x86_vmm_cpu_has_nx(void)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;

	__asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000U), "c"(0U));

	if (eax < 0x80000001U) {
		return false;
	}

	__asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001U), "c"(0U));

	return (edx & X86_CPUID_NX) != 0U;
}

/*
 * Routine:     x86_vmm_enable_nx
 * Purpose:
 *              Enable EFER.NXE and CR0.WP when supported so data mappings can
 *              be non-executable and read-only pages are enforced in ring 0.
 */
static bool
x86_vmm_enable_nx(void)
{
	uintptr_t cr0 = x86_vmm_read_cr0();

	if ((cr0 & X86_CR0_PG) == 0UL) {
		return false;
	}

	cr0 |= X86_CR0_WP;
	__asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

	if (!x86_vmm_cpu_has_nx()) {
		return true;
	}

	uint32_t low;
	uint32_t high;
	__asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(X86_MSR_EFER));

	uint64_t efer = ((uint64_t)high << 32U) | low;
	efer |= X86_EFER_NXE;

	__asm__ volatile("wrmsr" : : "c"(X86_MSR_EFER), "a"((uint32_t)efer), "d"((uint32_t)(efer >> 32U)) : "memory");

	return true;
}

/*
 * Routine:     x86_vmm_protect_identity_page
 * Purpose:
 *              Set one identity-mapped kernel page to its final writable and
 *              executable permissions after its large page has been split.
 */
static bool
x86_vmm_protect_image_page(uint64_t *root, uint64_t address, uint64_t physical, bool writable, bool executable)
{
	uint64_t *entry;
	uint64_t table_count = g_x86_vmm.table_count;

	if (!x86_vmm_root_get_l3_entry(root, address, false, false, &table_count, &entry)) {
		return false;
	}

	uint64_t descriptor = physical & X86_PTE_ADDRESS;
	descriptor |= X86_PTE_PRESENT;
	if (writable) {
		descriptor |= X86_PTE_WRITE;
	}

	if (g_x86_vmm.nx_enabled && !executable) {
		descriptor |= X86_PTE_NX;
	}

	*entry = descriptor;

	return true;
}

/*
 * Routine:     x86_vmm_protect_identity_range
 * Purpose:
 *              Apply one final kernel-image permission set to every page in
 *              a page-aligned physical section range.
 */
static bool
x86_vmm_protect_image_range(uint64_t *root, uint64_t alias_base, uint64_t start, uint64_t end, bool writable,
							bool executable)
{
	if (end < start || start < 0x100000ULL || end > VM_DIRECT_MAP_SIZE ||
		(start & (X86_VM_PAGE_SIZE - 1ULL)) != 0ULL || (end & (X86_VM_PAGE_SIZE - 1ULL)) != 0ULL) {
		return false;
	}

	for (uint64_t address = start; address < end; address += X86_VM_PAGE_SIZE) {
		if (!x86_vmm_protect_image_page(root, alias_base + address, address, writable, executable)) {
			return false;
		}
	}

	return true;
}

/*
 * Routine:     x86_vmm_protect_kernel_image
 * Purpose:
 *              Split large identity pages covering the kernel image, make
 *              the remaining direct identity map NX, and enforce section
 *              write and execute permissions.
 */
static bool
x86_vmm_protect_kernel_image_root(uint64_t root_index, uint64_t alias_base)
{
	uint64_t *root = g_x86_vmm.root;
	uint64_t *identity_pdpt;
	uint64_t *identity_pd;
	if (!x86_vmm_table_from_physical(root[root_index] & X86_PTE_ADDRESS, &identity_pdpt) ||
		!x86_vmm_table_from_physical(identity_pdpt[0] & X86_PTE_ADDRESS, &identity_pd)) {
		return false;
	}

	for (uint32_t index = 0U; index < 512U; index++) {
		identity_pd[index] |= g_x86_vmm.nx_enabled ? X86_PTE_NX : 0ULL;
	}

	uint64_t kernel_start = (uint64_t)(uintptr_t)__kernel_start;
	uint64_t kernel_end = (uint64_t)(uintptr_t)__kernel_end;
	uint32_t first = (uint32_t)(kernel_start / X86_VM_LARGE_PAGE_SIZE);
	uint32_t last = (uint32_t)((kernel_end - 1ULL) / X86_VM_LARGE_PAGE_SIZE);

	for (uint32_t index = first; index <= last; index++) {
		uint64_t large = identity_pd[index];
		if ((large & X86_PTE_LARGE) == 0ULL) {
			return false;
		}

		uint64_t *page_table;
		uint64_t physical;
		if (!x86_vmm_allocate_table(&page_table, &physical, &g_x86_vmm.table_count)) {
			return false;
		}

		uint64_t base = large & X86_PTE_LARGE_ADDRESS;
		uint64_t flags = X86_PTE_PRESENT | X86_PTE_WRITE;
		if (g_x86_vmm.nx_enabled) {
			flags |= X86_PTE_NX;
		}

		for (uint32_t page = 0U; page < 512U; page++) {
			page_table[page] = (base + (uint64_t)page * X86_VM_PAGE_SIZE) | flags;
		}

		identity_pd[index] = physical | X86_PTE_PRESENT | X86_PTE_WRITE;
	}

	uint64_t text_start = (uint64_t)(uintptr_t)__text_start & ~(X86_VM_PAGE_SIZE - 1ULL);
	uint64_t text_end = ((uint64_t)(uintptr_t)__text_end + X86_VM_PAGE_SIZE - 1ULL) & ~(X86_VM_PAGE_SIZE - 1ULL);
	uint64_t rodata_start = (uint64_t)(uintptr_t)__rodata_start & ~(X86_VM_PAGE_SIZE - 1ULL);
	uint64_t rodata_end = ((uint64_t)(uintptr_t)__rodata_end + X86_VM_PAGE_SIZE - 1ULL) & ~(X86_VM_PAGE_SIZE - 1ULL);
	uint64_t data_start = (uint64_t)(uintptr_t)__data_start & ~(X86_VM_PAGE_SIZE - 1ULL);
	uint64_t data_end = ((uint64_t)(uintptr_t)__data_end + X86_VM_PAGE_SIZE - 1ULL) & ~(X86_VM_PAGE_SIZE - 1ULL);
	uint64_t kernel_data_start = data_start < rodata_end ? rodata_end : data_start;

	return x86_vmm_protect_image_range(root, alias_base, text_start, text_end, false, true) &&
		x86_vmm_protect_image_range(root, alias_base, rodata_start, rodata_end, false, false) &&
		x86_vmm_protect_image_range(root, alias_base, kernel_data_start, data_end, true, false) &&
		x86_vmm_protect_image_range(root, alias_base,
			(uint64_t)(uintptr_t)__bss_start & ~(X86_VM_PAGE_SIZE - 1ULL),
			((uint64_t)(uintptr_t)__bss_end + X86_VM_PAGE_SIZE - 1ULL) & ~(X86_VM_PAGE_SIZE - 1ULL), true, false);
}

/*
 * Routine:     x86_vmm_protect_kernel_image
 * Purpose:
 *              Apply the kernel section permissions to both the low boot
 *              identity map and the permanent high direct-map alias.
 */
static bool
x86_vmm_protect_kernel_image(void)
{
	if (!x86_vmm_protect_kernel_image_root(0ULL, 0ULL) ||
		!x86_vmm_protect_kernel_image_root(256ULL, VMM_HIGHER_HALF_BASE)) {
		return false;
	}

	return true;
}

/*
 * Routine:     x86_vmm_ensure_root_slot
 * Purpose:
 *              Reserve a shared PML4 branch for kernel mappings that may be
 *              added after user address spaces copy the kernel half.
 */
static bool
x86_vmm_ensure_root_slot(uint64_t index)
{
	if (index >= 512ULL) {
		return false;
	}

	if ((g_x86_vmm.root[index] & X86_PTE_PRESENT) != 0ULL) {
		return true;
	}

	uint64_t physical;
	uint64_t *table;

	if (!x86_vmm_allocate_table(&table, &physical, &g_x86_vmm.table_count)) {
		return false;
	}

	(void)table;
	g_x86_vmm.root[index] = physical | X86_PTE_PRESENT | X86_PTE_WRITE;

	return true;
}

/*
 * Routine:     x86_vmm_table_from_physical
 * Purpose:
 *              Access a page-table page through the permanent x86-64 direct
 *              map, which remains valid before and after VMM initialization.
 */
bool
x86_vmm_table_from_physical(uint64_t physical, uint64_t **table)
{
	if (table == 0 || (physical & (X86_VM_PAGE_SIZE - 1ULL)) != 0ULL || physical >= VM_DIRECT_MAP_SIZE) {
		return false;
	}

	uint64_t virtual_address;

	if (!vmm_physical_to_higher_half(physical, &virtual_address)) {
		return false;
	}

	*table = (uint64_t *)(uintptr_t)virtual_address;

	return true;
}

/*
 * Routine:     x86_vmm_allocate_table
 * Purpose:
 *              Allocate one zero-filled physical page for a translation-table
 *              level and update the owning root's table count.
 */
bool
x86_vmm_allocate_table(uint64_t **table, uint64_t *physical, uint64_t *count)
{
	if (table == 0 || physical == 0 || count == 0 || !pmm_allocate_page(physical)) {
		return false;
	}

	if (!x86_vmm_table_from_physical(*physical, table)) {
		(void)pmm_free_page(*physical);
		return false;
	}

	(*count)++;

	return true;
}

/*
 * Routine:     x86_vmm_next_table
 * Purpose:
 *              Resolve one non-leaf page-table entry, allocating a child table
 *              when requested and preserving user access for lower mappings.
 */
static bool
x86_vmm_next_table(uint64_t *parent, uint64_t index, bool create, bool user, uint64_t *count, uint64_t **child)
{
	if (parent == 0 || index >= 512ULL || count == 0 || child == 0) {
		return false;
	}

	uint64_t descriptor = parent[index];

	if ((descriptor & X86_PTE_PRESENT) != 0ULL) {
		if ((descriptor & X86_PTE_LARGE) != 0ULL) {
			return false;
		}

		if (user && (descriptor & X86_PTE_USER) == 0ULL) {
			parent[index] = descriptor | X86_PTE_USER;
		}

		return x86_vmm_table_from_physical(descriptor & X86_PTE_ADDRESS, child);
	}

	if (!create) {
		return false;
	}

	uint64_t physical;

	if (!x86_vmm_allocate_table(child, &physical, count)) {
		return false;
	}

	parent[index] = physical | X86_PTE_PRESENT | X86_PTE_WRITE | (user ? X86_PTE_USER : 0ULL);

	return true;
}

/*
 * Routine:     x86_vmm_root_get_l3_entry
 * Purpose:
 *              Walk a four-level root to one 4 KiB PTE, optionally creating
 *              the intermediate tables required for that virtual address.
 */
bool
x86_vmm_root_get_l3_entry(uint64_t *root, uint64_t address, bool create, bool user, uint64_t *table_count,
							  uint64_t **entry)
{
	uint64_t upper = address >> 48U;
	uint64_t sign = (address >> 47U) & 1ULL;

	if (root == 0 || table_count == 0 || entry == 0 || (sign == 0ULL && upper != 0ULL) ||
		(sign != 0ULL && upper != 0xFFFFULL)) {
		return false;
	}

	uint64_t *pdpt;
	uint64_t *pd;
	uint64_t *pt;

	if (!x86_vmm_next_table(root, (address >> 39U) & X86_VM_INDEX_MASK, create, user, table_count, &pdpt) ||
		!x86_vmm_next_table(pdpt, (address >> 30U) & X86_VM_INDEX_MASK, create, user, table_count, &pd) ||
		!x86_vmm_next_table(pd, (address >> 21U) & X86_VM_INDEX_MASK, create, user, table_count, &pt)) {
		return false;
	}

	*entry = &pt[(address >> 12U) & X86_VM_INDEX_MASK];

	return true;
}

/*
 * Routine:     x86_vmm_invalidate_page
 * Purpose:
 *              Remove one virtual address from the local translation cache
 *              after a live page-table change.
 */
void
x86_vmm_invalidate_page(uint64_t address)
{
	__asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)address) : "memory");
}

/*
 * Routine:     x86_vmm_attributes
 * Purpose:
 *              Encode the protection and memory type shared by VMM mappings.
 */
static bool
x86_vmm_attributes(vmm_memory_type_t memory_type, vmm_protection_t protection, uint64_t *attributes)
{
	if (attributes == 0) {
		return false;
	}

	uint64_t value = X86_PTE_PRESENT;

	switch (protection) {
	case VMM_PROTECTION_READ_WRITE:
		value |= X86_PTE_WRITE;
		if (g_x86_vmm.nx_enabled) {
			value |= X86_PTE_NX;
		}
		break;

	case VMM_PROTECTION_READ_ONLY:
		if (g_x86_vmm.nx_enabled) {
			value |= X86_PTE_NX;
		}
		break;

	case VMM_PROTECTION_READ_EXECUTE:
		break;

	default:
		return false;
	}

	switch (memory_type) {
	case VMM_MEMORY_NORMAL:
		break;

	case VMM_MEMORY_DEVICE:
		value |= X86_PTE_CACHE_DISABLE | X86_PTE_WRITE_THROUGH;
		if (g_x86_vmm.nx_enabled) {
			value |= X86_PTE_NX;
		}
		break;

	default:
		return false;
	}

	*attributes = value;

	return true;
}

/*
 * Routine:     x86_vmm_make_user_descriptor
 * Purpose:
 *              Build a leaf descriptor with x86 user access, write, NX, and
 *              software copy-on-write state for a process mapping.
 */
bool
x86_vmm_make_user_descriptor(uint64_t physical, vm_user_protection_t protection, bool cow, uint64_t *descriptor)
{
	if (descriptor == 0 || (physical & (X86_VM_PAGE_SIZE - 1ULL)) != 0ULL || physical >= VM_DIRECT_MAP_SIZE) {
		return false;
	}

	uint64_t value = (physical & X86_PTE_ADDRESS) | X86_PTE_PRESENT | X86_PTE_USER;

	switch (protection) {
	case VM_USER_PROTECTION_READ_WRITE:
		if (!cow) {
			value |= X86_PTE_WRITE;
		}

		if (g_x86_vmm.nx_enabled) {
			value |= X86_PTE_NX;
		}
		break;

	case VM_USER_PROTECTION_READ_ONLY:
		if (g_x86_vmm.nx_enabled) {
			value |= X86_PTE_NX;
		}
		break;

	case VM_USER_PROTECTION_READ_EXECUTE:
		break;

	default:
		return false;
	}

	if (cow) {
		value |= X86_PTE_SOFTWARE_COW;
	}

	*descriptor = value;

	return true;
}

/*
 * Routine:     x86_vmm_protection_from_entry
 * Purpose:
 *              Decode the leaf write and execute bits into the public VMM
 *              protection value.
 */
static vmm_protection_t
x86_vmm_protection_from_entry(uint64_t entry)
{
	if ((entry & X86_PTE_WRITE) != 0ULL) {
		return VMM_PROTECTION_READ_WRITE;
	}

	if (g_x86_vmm.nx_enabled && (entry & X86_PTE_NX) == 0ULL) {
		return VMM_PROTECTION_READ_EXECUTE;
	}

	return VMM_PROTECTION_READ_ONLY;
}

/*
 * Routine:     x86_vmm_walk
 * Purpose:
 *              Resolve a virtual address in a page-table root and report the
 *              physical byte, leaf descriptor, and effective write access.
 */
static bool
x86_vmm_walk(uint64_t *root, uint64_t address, uint64_t *leaf, uint64_t *physical, bool *writable)
{
	if (root == 0) {
		return false;
	}

	uint64_t descriptor = root[(address >> 39U) & X86_VM_INDEX_MASK];
	if ((descriptor & X86_PTE_PRESENT) == 0ULL) {
		return false;
	}

	uint64_t *table;
	if (!x86_vmm_table_from_physical(descriptor & X86_PTE_ADDRESS, &table)) {
		return false;
	}

	descriptor = table[(address >> 30U) & X86_VM_INDEX_MASK];
	if ((descriptor & X86_PTE_PRESENT) == 0ULL) {
		return false;
	}

	if ((descriptor & X86_PTE_LARGE) != 0ULL) {
		if (leaf != 0) {
			*leaf = descriptor;
		}

		if (physical != 0) {
			*physical = (descriptor & 0x000FFFFFC0000000ULL) | (address & 0x3FFFFFFFULL);
		}

		if (writable != 0) {
			*writable = (descriptor & X86_PTE_WRITE) != 0ULL;
		}

		return true;
	}

	if (!x86_vmm_table_from_physical(descriptor & X86_PTE_ADDRESS, &table)) {
		return false;
	}

	descriptor = table[(address >> 21U) & X86_VM_INDEX_MASK];
	if ((descriptor & X86_PTE_PRESENT) == 0ULL) {
		return false;
	}

	if ((descriptor & X86_PTE_LARGE) != 0ULL) {
		if (leaf != 0) {
			*leaf = descriptor;
		}

		if (physical != 0) {
			*physical = (descriptor & X86_PTE_LARGE_ADDRESS) | (address & (X86_VM_LARGE_PAGE_SIZE - 1ULL));
		}

		if (writable != 0) {
			*writable = (descriptor & X86_PTE_WRITE) != 0ULL;
		}

		return true;
	}

	if (!x86_vmm_table_from_physical(descriptor & X86_PTE_ADDRESS, &table)) {
		return false;
	}

	descriptor = table[(address >> 12U) & X86_VM_INDEX_MASK];
	if ((descriptor & X86_PTE_PRESENT) == 0ULL) {
		return false;
	}

	if (leaf != 0) {
		*leaf = descriptor;
	}

	if (physical != 0) {
		*physical = (descriptor & X86_PTE_ADDRESS) | (address & (X86_VM_PAGE_SIZE - 1ULL));
	}

	if (writable != 0) {
		*writable = (descriptor & X86_PTE_WRITE) != 0ULL;
	}

	return true;
}

/*
 * Routine:     vmm_init
 * Purpose:
 *              Adopt the bootstrap root, enable write-protect/NX support, and
 *              reserve shared kernel PML4 branches for later mappings.
 */
bool
vmm_init(const platform_t *platform)
{
	kputln("vmm_init: begin");
	if (g_x86_vmm.enabled || platform == 0 || platform->memory_region_count == 0U ||
		(x86_vmm_read_cr4() & (X86_CR4_PAE | X86_CR4_LA57)) != X86_CR4_PAE || !x86_vmm_enable_nx()) {
		kputln("vmm_init: mode or NX setup failed");
		return false;
	}
	kprintf("vmm_init: CR0 write protection enabled, NX %s\n", x86_vmm_cpu_has_nx() ? "available" : "unavailable");

	uintptr_t root_physical;
	__asm__ volatile("mov %%cr3, %0" : "=r"(root_physical));
	root_physical &= (uintptr_t)X86_PTE_ADDRESS;

	memset(&g_x86_vmm, 0, sizeof(g_x86_vmm));
	g_x86_vmm.root_physical = root_physical;
	if (!x86_vmm_table_from_physical(root_physical, &g_x86_vmm.root)) {
		kputln("vmm_init: root access failed");
		return false;
	}
	kprintf("vmm_init: adopted root 0x%llx\n", (unsigned long long)root_physical);

	g_x86_vmm.table_count = 5ULL;
	g_x86_vmm.nx_enabled = x86_vmm_cpu_has_nx();
	g_x86_vmm.direct_map_enabled = true;
	g_x86_vmm.higher_half_enabled = true;
	g_x86_vmm.pointers_rebased = true;
	g_x86_vmm.enabled = true;

	if (!x86_vmm_ensure_root_slot(X86_KERNEL_PML4_INDEX(VM_KERN_BASE)) ||
		!x86_vmm_ensure_root_slot(X86_KERNEL_PML4_INDEX(VM_MMIO_BASE))) {
		kputln("vmm_init: kernel mapping slots failed");
		g_x86_vmm.enabled = false;
		return false;
	}

	if (g_x86_vmm.nx_enabled) {
		kputln("vmm_init: applying direct-map permissions");
		uint64_t *direct_pdpt;
		uint64_t *direct_pd;

		if (!x86_vmm_table_from_physical(g_x86_vmm.root[256] & X86_PTE_ADDRESS, &direct_pdpt) ||
			!x86_vmm_table_from_physical(direct_pdpt[0] & X86_PTE_ADDRESS, &direct_pd)) {
			g_x86_vmm.enabled = false;
			return false;
		}

		for (uint32_t index = 0U; index < 512U; index++) {
			direct_pd[index] |= X86_PTE_NX;
		}
	}

	if (!x86_vmm_protect_kernel_image()) {
		kputln("vmm_init: kernel image permission setup failed");
		g_x86_vmm.enabled = false;
		return false;
	}

	__asm__ volatile("mov %0, %%cr3" : : "r"(root_physical) : "memory");
	kputln("vmm_init: updated page tables activated through CR3");

	return true;
}

/*
 * Routine:     vmm_map_identity
 * Purpose:
 *              Confirm an early identity range is already covered by the
 *              bootstrap map before the allocator starts using it.
 */
bool
vmm_map_identity(uint64_t physical_address, uint64_t size, vmm_memory_type_t memory_type,
				 vmm_protection_t protection)
{
	if (memory_type != VMM_MEMORY_NORMAL || size == 0ULL || physical_address >= VM_DIRECT_MAP_SIZE ||
		size > VM_DIRECT_MAP_SIZE - physical_address) {
		return false;
	}

	uint64_t translated;

	return vmm_translate(physical_address, &translated) && translated == physical_address &&
		vmm_query_page(physical_address, &(vmm_page_mapping_t){0}) &&
		(protection != VMM_PROTECTION_READ_WRITE || vmm_translate_write(physical_address, &translated));
}

/*
 * Routine:     vmm_map_page
 * Purpose:
 *              Install one 4 KiB kernel-arena or MMIO mapping in the shared
 *              kernel root and invalidate the local address translation.
 */
bool
vmm_map_page(uint64_t virtual_address, uint64_t physical_address, vmm_memory_type_t memory_type,
			 vmm_protection_t protection)
{
	if (!g_x86_vmm.enabled || (virtual_address & (X86_VM_PAGE_SIZE - 1ULL)) != 0ULL ||
		(physical_address & (X86_VM_PAGE_SIZE - 1ULL)) != 0ULL || physical_address >= VM_DIRECT_MAP_SIZE ||
		!((virtual_address >= VM_KERN_BASE && virtual_address < VM_KERN_BASE + VM_KERN_SIZE) ||
		  (virtual_address >= VM_MMIO_BASE && virtual_address < VM_MMIO_BASE + VM_MMIO_SIZE))) {
		return false;
	}

	uint64_t attributes;
	if (!x86_vmm_attributes(memory_type, protection, &attributes)) {
		return false;
	}

	NXU_RLOCK_GUARD(&g_x86_vmm_lock);

	uint64_t *entry;
	if (!x86_vmm_root_get_l3_entry(g_x86_vmm.root, virtual_address, true, false, &g_x86_vmm.table_count, &entry) ||
		(*entry & X86_PTE_PRESENT) != 0ULL) {
		return false;
	}

	*entry = (physical_address & X86_PTE_ADDRESS) | attributes;
	x86_vmm_invalidate_page(virtual_address);

	return true;
}

/*
 * Routine:     vmm_unmap_page
 * Purpose:
 *              Remove one dynamic kernel mapping and optionally return its
 *              physical page address to the caller.
 */
bool
vmm_unmap_page(uint64_t virtual_address, uint64_t *physical_address)
{
	if (!g_x86_vmm.enabled ||
		!((virtual_address >= VM_KERN_BASE && virtual_address < VM_KERN_BASE + VM_KERN_SIZE) ||
		  (virtual_address >= VM_MMIO_BASE && virtual_address < VM_MMIO_BASE + VM_MMIO_SIZE))) {
		return false;
	}

	NXU_RLOCK_GUARD(&g_x86_vmm_lock);
	uint64_t *entry;

	if (!x86_vmm_root_get_l3_entry(g_x86_vmm.root, virtual_address, false, false, &g_x86_vmm.table_count, &entry) ||
		(*entry & X86_PTE_PRESENT) == 0ULL) {
		return false;
	}

	if (physical_address != 0) {
		*physical_address = *entry & X86_PTE_ADDRESS;
	}

	*entry = 0ULL;
	x86_vmm_invalidate_page(virtual_address);

	return true;
}

/*
 * Routine:     vmm_protect_page
 * Purpose:
 *              Replace the protection bits of an existing dynamic kernel
 *              mapping without changing its physical frame.
 */
bool
vmm_protect_page(uint64_t virtual_address, vmm_protection_t protection)
{
	uint64_t *entry;
	uint64_t attributes;

	if (!g_x86_vmm.enabled ||
		!((virtual_address >= VM_KERN_BASE && virtual_address < VM_KERN_BASE + VM_KERN_SIZE) ||
		  (virtual_address >= VM_MMIO_BASE && virtual_address < VM_MMIO_BASE + VM_MMIO_SIZE)) ||
		!x86_vmm_attributes(VMM_MEMORY_NORMAL, protection, &attributes)) {
		return false;
	}

	NXU_RLOCK_GUARD(&g_x86_vmm_lock);
	if (!x86_vmm_root_get_l3_entry(g_x86_vmm.root, virtual_address, false, false, &g_x86_vmm.table_count, &entry) ||
		(*entry & X86_PTE_PRESENT) == 0ULL) {
		return false;
	}

	*entry = (*entry & (X86_PTE_ADDRESS | X86_PTE_CACHE_DISABLE | X86_PTE_WRITE_THROUGH)) | attributes;
	x86_vmm_invalidate_page(virtual_address);

	return true;
}

/*
 * Routine:     vmm_query_page
 * Purpose:
 *              Describe a present 4 KiB, 2 MiB, or 1 GiB translation in the
 *              active kernel root.
 */
bool
vmm_query_page(uint64_t virtual_address, vmm_page_mapping_t *mapping)
{
	if (!g_x86_vmm.enabled || mapping == 0) {
		return false;
	}

	uint64_t entry;
	uint64_t physical;
	if (!x86_vmm_walk(g_x86_vmm.root, virtual_address, &entry, &physical, 0)) {
		return false;
	}

	mapping->physical_address = physical & ~(X86_VM_PAGE_SIZE - 1ULL);
	mapping->memory_type = (entry & (X86_PTE_CACHE_DISABLE | X86_PTE_WRITE_THROUGH)) != 0ULL ?
		VMM_MEMORY_DEVICE : VMM_MEMORY_NORMAL;
	mapping->protection = x86_vmm_protection_from_entry(entry);

	return true;
}

/*
 * Routine:     vmm_translate
 * Purpose:
 *              Resolve a virtual address through the active kernel page-table
 *              root and return its physical address including page offset.
 */
bool
vmm_translate(uint64_t virtual_address, uint64_t *physical_address)
{
	return g_x86_vmm.enabled && physical_address != 0 &&
		x86_vmm_walk(g_x86_vmm.root, virtual_address, 0, physical_address, 0);
}

/*
 * Routine:     vmm_translate_write
 * Purpose:
 *              Resolve a virtual address only when every translation level
 *              permits a supervisor write.
 */
bool
vmm_translate_write(uint64_t virtual_address, uint64_t *physical_address)
{
	bool writable;

	return g_x86_vmm.enabled && physical_address != 0 &&
		x86_vmm_walk(g_x86_vmm.root, virtual_address, 0, physical_address, &writable) && writable;
}

/*
 * Routine:     vmm_create_identity_stub
 * Purpose:
 *              Create a private identity-map root for a bounded low-memory
 *              transition region used before secondary CPUs join the kernel.
 */
bool
vmm_create_identity_stub(uint64_t physical_address, uint64_t size, uint64_t *root_physical)
{
	if (!g_x86_vmm.enabled || root_physical == 0 || size == 0ULL || physical_address >= VM_DIRECT_MAP_SIZE ||
		size > VM_DIRECT_MAP_SIZE - physical_address) {
		return false;
	}

	uint64_t *root;
	uint64_t root_pa;
	uint64_t count = 0ULL;
	if (!x86_vmm_allocate_table(&root, &root_pa, &count)) {
		return false;
	}

	for (uint64_t index = 256ULL; index < 512ULL; index++) {
		root[index] = g_x86_vmm.root[index];
	}

	uint64_t start = physical_address & ~(X86_VM_PAGE_SIZE - 1ULL);
	uint64_t end = (physical_address + size + X86_VM_PAGE_SIZE - 1ULL) & ~(X86_VM_PAGE_SIZE - 1ULL);

	for (uint64_t address = start; address < end; address += X86_VM_PAGE_SIZE) {
		uint64_t *entry;
		if (!x86_vmm_root_get_l3_entry(root, address, true, false, &count, &entry)) {
			return false;
		}
		*entry = address | X86_PTE_PRESENT | X86_PTE_WRITE;
	}

	*root_physical = root_pa;

	return true;
}

/*
 * Routine:     vmm_validate_kernel_permissions
 * Purpose:
 *              Confirm CR0 write protection is active and that the linked
 *              kernel text remains translatable after VM initialization.
 */
bool
vmm_validate_kernel_permissions(void)
{
	uint64_t physical;
	uint64_t text = (uint64_t)(uintptr_t)__text_start;
	uint64_t rodata = (uint64_t)(uintptr_t)__rodata_start;
	uint64_t data = (uint64_t)(uintptr_t)__data_start;
	uint64_t high_text;
	uint64_t high_rodata;
	uint64_t high_data;

	return g_x86_vmm.enabled && (x86_vmm_read_cr0() & X86_CR0_WP) != 0UL &&
		vmm_translate(text, &physical) && physical == text && !vmm_translate_write(text, &physical) &&
		vmm_translate(rodata, &physical) && !vmm_translate_write(rodata, &physical) &&
		vmm_translate(data, &physical) && vmm_translate_write(data, &physical) &&
		vmm_physical_to_higher_half(text, &high_text) && vmm_translate(high_text, &physical) && physical == text &&
		!vmm_translate_write(high_text, &physical) && vmm_physical_to_higher_half(rodata, &high_rodata) &&
		vmm_translate(high_rodata, &physical) && !vmm_translate_write(high_rodata, &physical) &&
		vmm_physical_to_higher_half(data, &high_data) && vmm_translate_write(high_data, &physical);
}

/*
 * Routine:     vmm_validate_linked_kernel_layout
 * Purpose:
 *              Check the linker-provided kernel and section bounds fit within
 *              the physical identity window used by this VM stage.
 */
bool
vmm_validate_linked_kernel_layout(void)
{
	uint64_t start = (uint64_t)(uintptr_t)__kernel_start;
	uint64_t end = (uint64_t)(uintptr_t)__kernel_end;
	uint64_t text_start = (uint64_t)(uintptr_t)__text_start;
	uint64_t text_end = (uint64_t)(uintptr_t)__text_end;
	uint64_t rodata_start = (uint64_t)(uintptr_t)__rodata_start;
	uint64_t rodata_end = (uint64_t)(uintptr_t)__rodata_end;

	return start >= 0x100000ULL && start <= text_start && text_start < text_end && text_end <= rodata_start &&
		rodata_start < rodata_end && rodata_end <= end && end <= VM_DIRECT_MAP_SIZE;
}

/*
 * Routine:     vmm_is_enabled
 * Purpose:
 *              Report whether the x86-64 VMM has adopted the active root.
 */
bool
vmm_is_enabled(void)
{
	return g_x86_vmm.enabled;
}

/*
 * Routine:     vmm_init_higher_half_alias
 * Purpose:
 *              Confirm the x86-64 direct map already supplies the kernel's
 *              higher-half physical alias.
 */
bool
vmm_init_higher_half_alias(void)
{
	return g_x86_vmm.enabled && g_x86_vmm.higher_half_enabled;
}

/*
 * Routine:     vmm_physical_to_higher_half
 * Purpose:
 *              Convert a physical address in the bootstrap window to its
 *              x86-64 direct-map alias.
 */
bool
vmm_physical_to_higher_half(uint64_t physical_address, uint64_t *virtual_address)
{
	if (virtual_address == 0 || physical_address >= VM_DIRECT_MAP_SIZE ||
		physical_address > UINT64_MAX - VMM_HIGHER_HALF_BASE) {
		return false;
	}

	*virtual_address = VMM_HIGHER_HALF_BASE + physical_address;

	return true;
}

/*
 * Routine:     vmm_higher_half_to_physical
 * Purpose:
 *              Convert an address in the x86-64 physical direct map back to
 *              its physical address.
 */
bool
vmm_higher_half_to_physical(uint64_t virtual_address, uint64_t *physical_address)
{
	if (physical_address == 0 || virtual_address < VMM_HIGHER_HALF_BASE ||
		virtual_address - VMM_HIGHER_HALF_BASE >= VM_DIRECT_MAP_SIZE) {
		return false;
	}

	*physical_address = virtual_address - VMM_HIGHER_HALF_BASE;

	return true;
}

/*
 * Routine:     vmm_kernel_address_to_physical
 * Purpose:
 *              Resolve an identity-linked kernel symbol or a direct-map
 *              pointer to its physical address.
 */
bool
vmm_kernel_address_to_physical(uint64_t kernel_address, uint64_t *physical_address)
{
	if (physical_address == 0) {
		return false;
	}

	if (kernel_address < VM_DIRECT_MAP_SIZE) {
		*physical_address = kernel_address;
		return true;
	}

	return vmm_higher_half_to_physical(kernel_address, physical_address);
}

/*
 * Routine:     vmm_is_higher_half_address
 * Purpose:
 *              Identify a canonical x86-64 address in the upper half.
 */
bool
vmm_is_higher_half_address(uint64_t virtual_address)
{
	return virtual_address >= VMM_HIGHER_HALF_BASE;
}

/*
 * Routine:     vmm_higher_half_enabled
 * Purpose:
 *              Report whether physical memory has a usable higher-half alias.
 */
bool
vmm_higher_half_enabled(void)
{
	return g_x86_vmm.higher_half_enabled;
}

/*
 * Routine:     vmm_map_higher_half_direct_map
 * Purpose:
 *              Validate that all platform RAM ranges are already covered by
 *              the bootstrap x86-64 direct map.
 */
bool
vmm_map_higher_half_direct_map(const platform_t *platform)
{
	return vmm_validate_higher_half_direct_map(platform);
}

/*
 * Routine:     vmm_validate_higher_half_direct_map
 * Purpose:
 *              Check physical-to-virtual translations for both ends of each
 *              usable platform RAM region.
 */
bool
vmm_validate_higher_half_direct_map(const platform_t *platform)
{
	if (!g_x86_vmm.enabled || platform == 0 || platform->memory_region_count == 0U) {
		return false;
	}

	for (uint32_t index = 0U; index < platform->memory_region_count; index++) {
		const platform_region_t *region = &platform->memory_regions[index];
		uint64_t start_alias;
		uint64_t end_alias;
		uint64_t physical;

		if (region->size == 0ULL || region->base > UINT64_MAX - region->size ||
			!vmm_physical_to_higher_half(region->base, &start_alias) ||
			!vmm_physical_to_higher_half(region->base + region->size - 1ULL, &end_alias) ||
			!vmm_translate(start_alias, &physical) || physical != region->base ||
			!vmm_translate(end_alias, &physical) || physical != region->base + region->size - 1ULL) {
			return false;
		}
	}

	return true;
}

/*
 * Routine:     vmm_higher_half_direct_map_enabled
 * Purpose:
 *              Report whether the direct-map branch is available.
 */
bool
vmm_higher_half_direct_map_enabled(void)
{
	return g_x86_vmm.direct_map_enabled;
}

/*
 * Routine:     vmm_rebase_kernel_pointers
 * Purpose:
 *              Mark the physical direct map available to allocator-owned
 *              tables while the linked kernel itself remains identity based.
 */
bool
vmm_rebase_kernel_pointers(void)
{
	if (!g_x86_vmm.enabled || !g_x86_vmm.direct_map_enabled) {
		return false;
	}

	g_x86_vmm.pointers_rebased = true;

	return true;
}

/*
 * Routine:     vmm_disable_ttbr0
 * Purpose:
 *              Report that x86-64 has no separate TTBR0 translation regime.
 */
bool
vmm_disable_ttbr0(void)
{
	return false;
}

/*
 * Routine:     vmm_ttbr0_disabled
 * Purpose:
 *              Report that the architecture has no TTBR0 state to disable.
 */
bool
vmm_ttbr0_disabled(void)
{
	return false;
}

/*
 * Routine:     vmm_get_ttbr1_root
 * Purpose:
 *              Return the physical root currently used by the x86-64 kernel.
 */
uint64_t
vmm_get_ttbr1_root(void)
{
	return g_x86_vmm.root_physical;
}

/*
 * Routine:     vmm_get_ttbr1_table_count
 * Purpose:
 *              Return the number of page-table pages owned by the kernel root.
 */
uint64_t
vmm_get_ttbr1_table_count(void)
{
	return g_x86_vmm.table_count;
}

/*
 * Routine:     vmm_dump
 * Purpose:
 *              Print the active x86-64 root and its bootstrap mapping limits.
 */
void
vmm_dump(void)
{
	kprintf("vmm_dump: root 0x%llx, tables %llu, NX %s, direct map %llu MiB\n",
			(unsigned long long)g_x86_vmm.root_physical, (unsigned long long)g_x86_vmm.table_count,
			g_x86_vmm.nx_enabled ? "on" : "off", (unsigned long long)(VM_DIRECT_MAP_SIZE / (1024ULL * 1024ULL)));
}

/*
 * Routine:     vmm_dump_higher_half
 * Purpose:
 *              Describe the x86-64 direct map in the kernel log.
 */
void
vmm_dump_higher_half(void)
{
	kprintf("vmm_dump_higher_half: direct map 0x%llx-0x%llx\n", (unsigned long long)VMM_HIGHER_HALF_BASE,
			(unsigned long long)(VMM_HIGHER_HALF_BASE + VM_DIRECT_MAP_SIZE - 1ULL));
}

/*
 * Routine:     vmm_lower_page_valid
 * Purpose:
 *              Check that an address is an aligned user-range page address.
 */
bool
vmm_lower_page_valid(uint64_t virtual_address)
{
	return virtual_address != 0ULL && virtual_address < VM_MAX_USER_ADDRESS &&
		(virtual_address & (X86_VM_PAGE_SIZE - 1ULL)) == 0ULL;
}

/*
 * Routine:     vmm_higher_page_valid
 * Purpose:
 *              Check that an address is a canonical aligned kernel page.
 */
bool
vmm_higher_page_valid(uint64_t virtual_address)
{
	return vmm_is_higher_half_address(virtual_address) &&
		(virtual_address & (X86_VM_PAGE_SIZE - 1ULL)) == 0ULL;
}

/*
 * Routine:     vmm_physical_page_valid
 * Purpose:
 *              Check that a page-aligned frame is inside the current physical
 *              direct map and can be addressed by page tables.
 */
bool
vmm_physical_page_valid(uint64_t physical_address)
{
	return (physical_address & (X86_VM_PAGE_SIZE - 1ULL)) == 0ULL && physical_address < VM_DIRECT_MAP_SIZE;
}
