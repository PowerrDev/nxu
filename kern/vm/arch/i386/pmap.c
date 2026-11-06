/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/vm/arch/i386/pmap.c
 *
 * i386 (PAE) pmap backend: the architecture-specific half of the user address
 * space, implementing the kern/vm/pmap.h contract against the 32-bit PAE
 * translation tables managed by kern/i386/pmap.c. The neutral layer
 * (vm/address_space.c) owns ownership, reference counting, COW policy and
 * bookkeeping and reaches translations only through this contract.
 *
 * Each space owns a flattened 2048-entry directory (four 512-entry PAE
 * directories) plus a PDPT page; root_physical names the PDPT (loaded into
 * CR3). Directory entries PMAP_KERNEL_FIRST_PDE.. mirror the master kernel PDEs
 * and are never freed here. User pages carry PTE_USER; a copy-on-write page is
 * hardware read-only with PTE_SOFTWARE_COW set.
 *
 * TLB maintenance is this CPU's only (local invlpg): a mapping changed while
 * another CPU runs another thread of the same process is not shot down there,
 * matching the port's prior behaviour. Cross-CPU shootdown is a later
 * improvement; the `active` set is accepted but not yet used.
 */

#include <kern/vm/pmap.h>

#include <kern/i386/pmap.h>
#include <kern/machine/vm_param.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VM_USER_NULL_GUARD_SIZE PMAP_PAGE_SIZE

/* ------------------------------------------------------------------------- */
/* PTE construction and parsing.                                             */
/* ------------------------------------------------------------------------- */

static bool
pmap_user_flags(vm_prot_t protection, bool cow, pmap_entry_t *flags)
{
	pmap_entry_t value = PTE_PRESENT | PTE_USER | pmap_data_flags();

	switch (protection) {
	case VM_PROT_RW:
		value |= PTE_WRITE;
		break;

	case VM_PROT_RO:
		break;

	case VM_PROT_RX:
		value = (value & ~(pmap_entry_t)PTE_NX) | PTE_SOFTWARE_EXEC;
		break;

	default:
		return false;
	}

	/* A copy-on-write page is read-only in hardware with the software bit set. */
	if (cow) {
		value = (value & ~(pmap_entry_t)PTE_WRITE) | PTE_SOFTWARE_COW;
	}

	*flags = value;
	return true;
}

static bool
pmap_user_protection(pmap_entry_t entry, vm_prot_t *protection)
{
	if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) return false;

	if ((entry & PTE_WRITE) != 0U)
		*protection = VM_PROT_RW;
	else
		*protection = (entry & PTE_SOFTWARE_EXEC) != 0U ? VM_PROT_RX : VM_PROT_RO;

	return true;
}

/* A page-aligned address inside the mappable part of user space. */
static bool
pmap_user_va_valid(uint64_t va)
{
	return (va & PMAP_PAGE_MASK) == 0ULL && va >= VM_USER_NULL_GUARD_SIZE && va < VM_MAX_USER_ADDRESS;
}

/* A physical page a user mapping may name: page-aligned, inside the direct map. */
static bool
pmap_user_pa_valid(uint64_t pa)
{
	return (pa & PMAP_PAGE_MASK) == 0ULL && pa < VM_DIRECT_MAP_SIZE;
}

static pmap_entry_t *
pmap_root_directory(const pmap_t *pmap)
{
	return (pmap_entry_t *)pmap->root;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle.                                                                */
/* ------------------------------------------------------------------------- */

bool
pmap_create(pmap_t *pmap)
{
	if (pmap == 0 || !pmap_initialized() || pmap->root != 0 || pmap->root_physical != 0ULL ||
		pmap->table_count != 0ULL) {
		return false;
	}

	uint32_t directory_physical;
	pmap_entry_t *directory;

	if (!pmap_root_alloc(&directory, &directory_physical)) return false;

	const pmap_entry_t *master = pmap_kernel_directory();

	for (uint32_t index = PMAP_KERNEL_FIRST_PDE; index < PMAP_DIRECTORY_ENTRIES; index++) {
		directory[index] = master[index];
	}

	pmap->root = (uint64_t *)directory;
	pmap->root_physical = directory_physical;
	pmap->table_count = PMAP_ROOT_PAGES;

	return true;
}

bool
pmap_destroy(pmap_t *pmap)
{
	if (pmap == 0 || pmap->root == 0 || !pmap_initialized()) return false;

	pmap_entry_t *directory = pmap_root_directory(pmap);

	for (uint32_t index = 0U; index < PMAP_KERNEL_FIRST_PDE; index++) {
		if ((directory[index] & PTE_PRESENT) != 0U) pmap_table_free(directory[index] & PTE_FRAME);
	}

	pmap_root_free((uint32_t)pmap->root_physical);

	memset(pmap, 0, sizeof(*pmap));

	return true;
}

/* ------------------------------------------------------------------------- */
/* Activation (CR3 load).                                                    */
/* ------------------------------------------------------------------------- */

bool
pmap_activate(pmap_t *pmap)
{
	if (pmap == 0 || pmap->root == 0 || pmap->root_physical == 0ULL || pmap->table_count == 0ULL ||
		!pmap_initialized()) {
		return false;
	}

	pmap_load_directory((uint32_t)pmap->root_physical);

	return true;
}

bool
pmap_deactivate(void)
{
	if (!pmap_initialized()) return false;

	pmap_load_directory(pmap_kernel_directory_physical());

	return true;
}

/* ------------------------------------------------------------------------- */
/* Single-page operations.                                                   */
/* ------------------------------------------------------------------------- */

bool
pmap_enter(pmap_t *pmap, uint64_t va, uint64_t pa, vm_prot_t prot, bool cow, const nxu_cpuset_t *active)
{
	(void)active;

	pmap_entry_t flags;

	if (pmap == 0 || pmap->root == 0 || !pmap_user_va_valid(va) || !pmap_user_pa_valid(pa) ||
		!pmap_user_flags(prot, cow, &flags)) {
		return false;
	}

	pmap_entry_t *entry;

	if (!pmap_get_entry(pmap_root_directory(pmap), (uint32_t)va, true, true, &pmap->table_count, &entry)) {
		return false;
	}

	/* Never replace an existing mapping implicitly (use pmap_replace). */
	if ((*entry & PTE_PRESENT) != 0U) return false;

	pmap_entry_store(entry, ((pmap_entry_t)pa & PTE_FRAME) | flags);

	/* A not-present entry is never cached, but stay strictly correct against
	 * any negative caching on the active space. */
	pmap_invalidate_page((uint32_t)va);

	return true;
}

bool
pmap_remove(pmap_t *pmap, uint64_t va, const nxu_cpuset_t *active)
{
	(void)active;

	if (pmap == 0 || pmap->root == 0 || !pmap_user_va_valid(va)) return false;

	pmap_entry_t *entry;

	if (!pmap_get_entry(pmap_root_directory(pmap), (uint32_t)va, false, true, 0, &entry) ||
		(*entry & PTE_PRESENT) == 0U) {
		return false;
	}

	pmap_entry_store(entry, 0ULL);
	pmap_invalidate_page((uint32_t)va);

	return true;
}

bool
pmap_protect(pmap_t *pmap, uint64_t va, vm_prot_t prot, const nxu_cpuset_t *active)
{
	(void)active;

	/* Phase 0/1 narrowing: make a writable or read-only page read-only and drop
	 * any COW sharing; never widen and never touch an executable page. */
	if (pmap == 0 || pmap->root == 0 || prot != VM_PROT_RO || !pmap_user_va_valid(va)) return false;

	pmap_entry_t *entry;
	vm_prot_t current;

	if (!pmap_get_entry(pmap_root_directory(pmap), (uint32_t)va, false, true, 0, &entry) ||
		!pmap_user_protection(pmap_entry_load(entry), &current) || current == VM_PROT_RX) {
		return false;
	}

	pmap_entry_store(entry, pmap_entry_load(entry) & ~((pmap_entry_t)PTE_WRITE | (pmap_entry_t)PTE_SOFTWARE_COW));
	pmap_invalidate_page((uint32_t)va);

	return true;
}

bool
pmap_replace(pmap_t *pmap, uint64_t va, uint64_t new_pa, vm_prot_t prot, bool cow, const nxu_cpuset_t *active)
{
	(void)active;

	pmap_entry_t flags;

	if (pmap == 0 || pmap->root == 0 || !pmap_user_va_valid(va) || !pmap_user_pa_valid(new_pa) ||
		!pmap_user_flags(prot, cow, &flags)) {
		return false;
	}

	pmap_entry_t *entry;

	if (!pmap_get_entry(pmap_root_directory(pmap), (uint32_t)va, false, true, 0, &entry) ||
		(*entry & PTE_PRESENT) == 0U) {
		return false;
	}

	pmap_entry_store(entry, ((pmap_entry_t)new_pa & PTE_FRAME) | flags);
	pmap_invalidate_page((uint32_t)va);

	return true;
}

bool
pmap_query(const pmap_t *pmap, uint64_t va, vm_user_page_mapping_t *out)
{
	if (pmap == 0 || pmap->root == 0 || out == 0 || va < VM_USER_NULL_GUARD_SIZE || va >= VM_MAX_USER_ADDRESS) {
		return false;
	}

	pmap_entry_t *entry;

	if (!pmap_get_entry((pmap_entry_t *)pmap->root, (uint32_t)va, false, true, 0, &entry)) return false;

	pmap_entry_t value = pmap_entry_load(entry);

	if (!pmap_user_protection(value, &out->protection)) return false;

	out->physical_address = value & PTE_FRAME;
	out->cow = (value & PTE_SOFTWARE_COW) != 0U;

	return true;
}

bool
pmap_translate(const pmap_t *pmap, uint64_t va, bool write, uint64_t *pa)
{
	if (pmap == 0 || pmap->root == 0 || pa == 0 || va < VM_USER_NULL_GUARD_SIZE || va >= VM_MAX_USER_ADDRESS) {
		return false;
	}

	uint32_t address = (uint32_t)va;
	const pmap_entry_t *directory = (const pmap_entry_t *)pmap->root;
	pmap_entry_t directory_entry = pmap_entry_load(&directory[pmap_directory_index(address)]);

	if ((directory_entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
		(directory_entry & PTE_LARGE) != 0U) {
		return false;
	}

	pmap_entry_t entry = pmap_entry_load(&pmap_table_pointer(directory_entry & PTE_FRAME)[pmap_table_index(address)]);

	if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) return false;

	if (write && (entry & PTE_WRITE) == 0U) return false;

	*pa = (uint64_t)(entry & PTE_FRAME) | (address & PMAP_PAGE_MASK);

	return true;
}

/* ------------------------------------------------------------------------- */
/* COW / fork helpers.                                                       */
/* ------------------------------------------------------------------------- */

bool
pmap_copy_page(uint64_t dst_pa, uint64_t src_pa)
{
	if (!pmap_user_pa_valid(dst_pa) || !pmap_user_pa_valid(src_pa)) return false;

	memcpy((void *)pmap_table_pointer((uint32_t)dst_pa), (const void *)pmap_table_pointer((uint32_t)src_pa),
		   PMAP_PAGE_SIZE);

	return true;
}

bool
pmap_enumerate(const pmap_t *pmap, pmap_leaf_t *out, uint64_t cap, uint64_t *count)
{
	if (pmap == 0 || pmap->root == 0 || out == 0 || count == 0) return false;

	const pmap_entry_t *directory = (const pmap_entry_t *)pmap->root;
	uint64_t n = 0ULL;

	for (uint32_t index = 0U; index < PMAP_KERNEL_FIRST_PDE; index++) {
		pmap_entry_t directory_entry = directory[index];

		if ((directory_entry & PTE_PRESENT) == 0U || (directory_entry & PTE_LARGE) != 0U) continue;

		pmap_entry_t *table = pmap_table_pointer(directory_entry & PTE_FRAME);

		for (uint32_t slot = 0U; slot < PMAP_TABLE_ENTRIES; slot++) {
			pmap_entry_t value = pmap_entry_load(&table[slot]);
			vm_prot_t protection;

			if (!pmap_user_protection(value, &protection)) continue;

			if (n >= cap) return false;

			out[n].va = ((uint64_t)index << 21U) | ((uint64_t)slot << 12U);
			out[n].pa = value & PTE_FRAME;
			out[n].prot = protection;
			out[n].cow = (value & PTE_SOFTWARE_COW) != 0U;
			n++;
		}
	}

	*count = n;

	return true;
}

uint64_t
pmap_release_user_pages(pmap_t *pmap, void (*free_page)(uint64_t physical_address))
{
	if (pmap == 0 || pmap->root == 0 || free_page == 0) return 0ULL;

	pmap_entry_t *directory = pmap_root_directory(pmap);
	uint64_t released = 0ULL;

	for (uint32_t index = 0U; index < PMAP_KERNEL_FIRST_PDE; index++) {
		if ((directory[index] & PTE_PRESENT) == 0U || (directory[index] & PTE_LARGE) != 0U) continue;

		pmap_entry_t *table = pmap_table_pointer(directory[index] & PTE_FRAME);

		for (uint32_t slot = 0U; slot < PMAP_TABLE_ENTRIES; slot++) {
			if ((table[slot] & PTE_PRESENT) == 0U) continue;

			free_page(table[slot] & PTE_FRAME);
			pmap_entry_store(&table[slot], 0ULL);
			released++;
		}
	}

	/* One bulk flush for the whole space (teardown). */
	pmap_flush_tlb();

	return released;
}

uint64_t
pmap_root_physical(const pmap_t *pmap)
{
	return pmap == 0 ? 0ULL : pmap->root_physical;
}

uint64_t
pmap_table_count(const pmap_t *pmap)
{
	return pmap == 0 ? 0ULL : pmap->table_count;
}
