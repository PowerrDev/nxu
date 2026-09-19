/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/address_space_i386.c
 *
 * i386 implementation of vm/address_space.h: per-task user address spaces.
 * The arm64 implementation (vm/address_space.c) is not built for i386.
 *
 * An address space is one page directory (a single pmm page). Its user half,
 * PDEs 0..767, is private and filled with page tables allocated from pmm on
 * demand; its kernel half, PDEs 768..1023, is a copy of the master kernel
 * directory made at creation. That copy stays correct forever because the
 * kernel PDEs are final once pmap_init() has run (see pmap.h): later kernel
 * mappings only change leaf entries of page tables both directories share.
 *
 * Activation loads CR3. Mapping changes to the active space are followed by
 * invlpg; changes to an inactive space need no invalidation, because loading
 * CR3 flushes everything non-global (no page is global here).
 *
 * User pages carry PTE_USER. There is no NX bit: VM_USER_PROTECTION_READ_ONLY
 * and VM_USER_PROTECTION_READ_EXECUTE both become a read-only user page, and
 * the requested distinction is remembered in PTE_SOFTWARE_EXEC so queries
 * (the ELF loader checks the entry page is READ_EXECUTE) report it back.
 * Ring 0 honours read-only pages too: CR0.WP is set.
 *
 * The first page is never mappable (null guard) and nothing at or above
 * VM_MAX_USER_ADDRESS is: user mappings cannot touch the kernel half.
 */

#include <vm/address_space.h>

#include <mach/i386/pmap.h>

#include <kern/lock.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VM_USER_NULL_GUARD_SIZE PMAP_PAGE_SIZE

static vm_address_space_t *g_active_space;

static bool vm_user_entry_flags(vm_user_protection_t protection, uint32_t *flags)
{
	uint32_t value = PTE_PRESENT | PTE_USER;

	switch (protection) {
	case VM_USER_PROTECTION_READ_WRITE:
		value |= PTE_WRITE;
		break;

	case VM_USER_PROTECTION_READ_ONLY:
		break;

	case VM_USER_PROTECTION_READ_EXECUTE:
		value |= PTE_SOFTWARE_EXEC;
		break;

	default:
		return false;
	}

	*flags = value;
	return true;
}

static bool vm_user_entry_protection(uint32_t entry, vm_user_protection_t *protection)
{
	if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) return false;

	if ((entry & PTE_WRITE) != 0U) *protection = VM_USER_PROTECTION_READ_WRITE;
	else *protection = (entry & PTE_SOFTWARE_EXEC) != 0U ? VM_USER_PROTECTION_READ_EXECUTE : VM_USER_PROTECTION_READ_ONLY;

	return true;
}

/* A page-aligned address inside the mappable part of user space. */
static bool vm_user_page_valid(uint64_t virtual_address)
{
	return
		(virtual_address & PMAP_PAGE_MASK) == 0ULL &&
		virtual_address >= VM_USER_NULL_GUARD_SIZE &&
		virtual_address < VM_MAX_USER_ADDRESS;
}

/* Physical pages a user mapping may name: real, page-aligned, inside the direct map. */
static bool vm_user_physical_valid(uint64_t physical_address)
{
	return (physical_address & PMAP_PAGE_MASK) == 0ULL && physical_address < VM_DIRECT_MAP_SIZE;
}

static bool vm_address_space_translate(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	bool write,
	uint64_t *physical_address
)
{
	if (
		!vm_address_space_is_active(space) ||
		physical_address == 0 ||
		virtual_address < VM_USER_NULL_GUARD_SIZE ||
		virtual_address >= VM_MAX_USER_ADDRESS
	) {
		return false;
	}

	uint32_t address = (uint32_t)virtual_address;
	uint32_t directory_entry = ((const uint32_t *)space->root)[pmap_directory_index(address)];

	if ((directory_entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) || (directory_entry & PTE_LARGE) != 0U) return false;

	uint32_t entry = pmap_table_pointer(directory_entry & PTE_FRAME)[pmap_table_index(address)];

	if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) return false;
	if (write && (entry & PTE_WRITE) == 0U) return false;

	*physical_address = (uint64_t)(entry & PTE_FRAME) | (address & PMAP_PAGE_MASK);
	return true;
}

const vm_address_space_t *vm_address_space_current(void)
{
	return g_active_space;
}

bool vm_address_space_create(vm_address_space_t *space)
{
	if (
		space == 0 ||
		!pmap_initialized() ||
		space->root != 0 ||
		space->root_physical != 0ULL ||
		space->table_count != 0ULL ||
		space->active
	) {
		return false;
	}

	memset(space, 0, sizeof(*space));

	uint32_t directory_physical;

	if (!pmap_table_alloc(&directory_physical)) return false;

	uint32_t *directory = pmap_table_pointer(directory_physical);
	const uint32_t *master = pmap_kernel_directory();

	for (uint32_t index = PMAP_KERNEL_FIRST_PDE; index < PMAP_TABLE_ENTRIES; index++) directory[index] = master[index];

	space->root = (uint64_t *)directory;
	space->root_physical = directory_physical;
	space->table_count = 1ULL;

	return true;
}

bool vm_address_space_activate(vm_address_space_t *space)
{
	if (
		space == 0 ||
		space->root == 0 ||
		space->root_physical == 0ULL ||
		space->table_count == 0ULL ||
		!pmap_initialized()
	) {
		return false;
	}

	if (g_active_space == space) return true;

	if (g_active_space != 0) g_active_space->active = false;

	pmap_load_directory((uint32_t)space->root_physical);

	space->active = true;
	g_active_space = space;

	return true;
}

bool vm_address_space_deactivate(void)
{
	if (!pmap_initialized()) return false;

	if (g_active_space != 0) g_active_space->active = false;

	pmap_load_directory(pmap_kernel_directory_physical());

	g_active_space = 0;
	return true;
}

bool vm_address_space_map_page(
	vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t physical_address,
	vm_user_protection_t protection
)
{
	uint32_t flags;

	if (
		space == 0 ||
		space->root == 0 ||
		!vm_user_page_valid(virtual_address) ||
		!vm_user_physical_valid(physical_address) ||
		!vm_user_entry_flags(protection, &flags)
	) {
		return false;
	}

	nxu_spin_lock(&space->lock);

	uint32_t *entry;

	if (!pmap_get_entry((uint32_t *)space->root, (uint32_t)virtual_address, true, true, &space->table_count, &entry)) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	if ((*entry & PTE_PRESENT) != 0U) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	*entry = (uint32_t)physical_address | flags;

	/* A not-present entry is never cached, but stay strictly correct for the active space. */
	if (space == g_active_space) pmap_invalidate_page((uint32_t)virtual_address);

	nxu_spin_unlock(&space->lock);

	return true;
}

bool vm_address_space_unmap_page(vm_address_space_t *space, uint64_t virtual_address)
{
	if (space == 0 || space->root == 0 || !vm_user_page_valid(virtual_address)) return false;

	nxu_spin_lock(&space->lock);

	uint32_t *entry;

	if (!pmap_get_entry((uint32_t *)space->root, (uint32_t)virtual_address, false, true, 0, &entry) || (*entry & PTE_PRESENT) == 0U) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	*entry = 0U;

	if (space == g_active_space) pmap_invalidate_page((uint32_t)virtual_address);

	nxu_spin_unlock(&space->lock);

	return true;
}

bool vm_address_space_query_page(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	vm_user_page_mapping_t *mapping
)
{
	if (
		space == 0 ||
		space->root == 0 ||
		mapping == 0 ||
		virtual_address < VM_USER_NULL_GUARD_SIZE ||
		virtual_address >= VM_MAX_USER_ADDRESS
	) {
		return false;
	}

	/* Logically const, but the lock keeps a concurrent map/unmap's entry write whole. */
	nxu_spinlock_t *lock = (nxu_spinlock_t *)&space->lock;

	nxu_spin_lock(lock);

	uint32_t *entry;

	if (!pmap_get_entry((uint32_t *)space->root, (uint32_t)virtual_address, false, true, 0, &entry)) {
		nxu_spin_unlock(lock);
		return false;
	}

	uint32_t value = *entry;

	nxu_spin_unlock(lock);

	if (!vm_user_entry_protection(value, &mapping->protection)) return false;

	mapping->physical_address = value & PTE_FRAME;
	return true;
}

bool vm_address_space_translate_read(const vm_address_space_t *space, uint64_t virtual_address, uint64_t *physical_address)
{
	return vm_address_space_translate(space, virtual_address, false, physical_address);
}

bool vm_address_space_translate_write(const vm_address_space_t *space, uint64_t virtual_address, uint64_t *physical_address)
{
	return vm_address_space_translate(space, virtual_address, true, physical_address);
}

bool vm_address_space_is_active(const vm_address_space_t *space)
{
	return space != 0 && space == g_active_space && space->active;
}

uint64_t vm_address_space_root_physical(const vm_address_space_t *space)
{
	return space == 0 ? 0ULL : space->root_physical;
}

uint64_t vm_address_space_table_count(const vm_address_space_t *space)
{
	return space == 0 ? 0ULL : space->table_count;
}

uint64_t vm_address_space_release_pages(vm_address_space_t *space)
{
	if (space == 0 || space->root == 0) return 0ULL;

	uint32_t *directory = (uint32_t *)space->root;
	uint64_t released = 0ULL;

	nxu_spin_lock(&space->lock);

	for (uint32_t index = 0U; index < PMAP_KERNEL_FIRST_PDE; index++) {
		if ((directory[index] & PTE_PRESENT) == 0U) continue;

		uint32_t *table = pmap_table_pointer(directory[index] & PTE_FRAME);

		for (uint32_t slot = 0U; slot < PMAP_TABLE_ENTRIES; slot++) {
			if ((table[slot] & PTE_PRESENT) == 0U) continue;

			(void)pmm_free_page(table[slot] & PTE_FRAME);
			table[slot] = 0U;
			released++;
		}
	}

	/* Every translation this space had is gone; make the CPU forget them too. */
	if (space == g_active_space) pmap_flush_tlb();

	nxu_spin_unlock(&space->lock);

	return released;
}

bool vm_address_space_destroy(vm_address_space_t *space)
{
	if (space == 0 || space->root == 0 || !pmap_initialized()) return false;

	/* Never free the directory the CPU is still walking. */
	if (space == g_active_space && !vm_address_space_deactivate()) return false;

	uint32_t *directory = (uint32_t *)space->root;

	for (uint32_t index = 0U; index < PMAP_KERNEL_FIRST_PDE; index++) {
		if ((directory[index] & PTE_PRESENT) != 0U) pmap_table_free(directory[index] & PTE_FRAME);
	}

	pmap_table_free((uint32_t)space->root_physical);

	memset(space, 0, sizeof(*space));
	return true;
}
