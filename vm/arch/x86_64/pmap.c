/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/arch/x86_64/pmap.c
 *
 * x86-64 pmap backend: the architecture-specific half of the user address
 * space, implementing the vm/pmap.h contract against the four-level (PML4)
 * translation tables managed by vm/x86_64/vmm.c. The neutral layer
 * (vm/address_space.c) owns ownership, reference counting, COW policy and
 * bookkeeping and reaches translations only through this contract.
 *
 * Each space owns a private PML4 whose upper half (entries 256..511) mirrors
 * the shared kernel branches held in g_x86_vmm.root; root_physical names the
 * PML4 (loaded into CR3). User leaves carry X86_PTE_USER; a copy-on-write page
 * is hardware read-only with X86_PTE_SOFTWARE_COW set (see
 * x86_vmm_make_user_descriptor).
 *
 * TLB maintenance is this CPU's only (local invlpg, or a CR3 reload for the
 * bulk teardown flush): this port runs one CPU, so a local flush is globally
 * coherent. The `active` cpuset is accepted but, as on i386, not yet used for
 * cross-CPU shootdown.
 */

#include <vm/pmap.h>

#include <vm/x86_64/vmm_internal.h>

#include <kern/machine/vm_param.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define X86_USER_NULL_GUARD_SIZE X86_VM_PAGE_SIZE
#define X86_USER_TABLE_ENTRIES 512U
#define X86_KERNEL_FIRST_PML4_INDEX 256U

/* ------------------------------------------------------------------------- */
/* Helpers.                                                                  */
/* ------------------------------------------------------------------------- */

/* A page-aligned address inside the mappable part of user space. */
static bool
pmap_user_va_valid(uint64_t va)
{
	return (va & (X86_VM_PAGE_SIZE - 1ULL)) == 0ULL && va >= X86_USER_NULL_GUARD_SIZE && va < VM_MAX_USER_ADDRESS;
}

/* A physical page a user mapping may name: page-aligned, inside the direct map. */
static bool
pmap_user_pa_valid(uint64_t pa)
{
	return (pa & (X86_VM_PAGE_SIZE - 1ULL)) == 0ULL && pa < VM_DIRECT_MAP_SIZE;
}

/* Resolve a present non-large table entry to its direct-map pointer. */
static bool
pmap_table_at_entry(uint64_t descriptor, uint64_t **table)
{
	return (descriptor & X86_PTE_PRESENT) != 0ULL && (descriptor & X86_PTE_LARGE) == 0ULL &&
		x86_vmm_table_from_physical(descriptor & X86_PTE_ADDRESS, table);
}

/* Decode a present user leaf's protection; false if not a present user leaf. */
static bool
pmap_user_protection(uint64_t descriptor, vm_prot_t *protection)
{
	if ((descriptor & (X86_PTE_PRESENT | X86_PTE_USER)) != (X86_PTE_PRESENT | X86_PTE_USER)) return false;

	if ((descriptor & X86_PTE_WRITE) != 0ULL) {
		*protection = VM_PROT_RW;
	} else if (g_x86_vmm.nx_enabled && (descriptor & X86_PTE_NX) == 0ULL) {
		*protection = VM_PROT_RX;
	} else {
		*protection = VM_PROT_RO;
	}

	return true;
}

/* Flush the whole local TLB (non-global) by reloading the active CR3. */
static void
pmap_flush_local(void)
{
	uintptr_t cr3;

	__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
	__asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* ------------------------------------------------------------------------- */
/* Lifecycle.                                                                */
/* ------------------------------------------------------------------------- */

bool
pmap_create(pmap_t *pmap)
{
	if (pmap == 0 || !g_x86_vmm.enabled || pmap->root != 0 || pmap->root_physical != 0ULL ||
		pmap->table_count != 0ULL) {
		return false;
	}

	if (!x86_vmm_allocate_table(&pmap->root, &pmap->root_physical, &pmap->table_count)) {
		memset(pmap, 0, sizeof(*pmap));
		return false;
	}

	/* Mirror the shared kernel-half branches into the new user root. */
	for (uint32_t index = X86_KERNEL_FIRST_PML4_INDEX; index < X86_USER_TABLE_ENTRIES; index++) {
		pmap->root[index] = g_x86_vmm.root[index];
	}

	return true;
}

/*
 * Free the user-owned translation tables below one PML4 branch, without
 * walking into the shared kernel half. Leaf data pages are already released by
 * the core (pmap_release_user_pages); this frees only the table structure.
 */
static void
pmap_free_table_tree(uint64_t *table, uint32_t level)
{
	for (uint32_t index = 0U; index < X86_USER_TABLE_ENTRIES; index++) {
		uint64_t descriptor = table[index];

		if ((descriptor & X86_PTE_PRESENT) == 0ULL || level == 1U || (descriptor & X86_PTE_LARGE) != 0ULL) {
			continue;
		}

		uint64_t *child;
		if (pmap_table_at_entry(descriptor, &child)) {
			pmap_free_table_tree(child, level - 1U);
		}

		(void)pmm_free_page(descriptor & X86_PTE_ADDRESS);
		table[index] = 0ULL;
	}
}

bool
pmap_destroy(pmap_t *pmap)
{
	if (pmap == 0 || pmap->root == 0 || !g_x86_vmm.enabled) return false;

	for (uint32_t index = 0U; index < X86_KERNEL_FIRST_PML4_INDEX; index++) {
		uint64_t *level3;

		if (!pmap_table_at_entry(pmap->root[index], &level3)) continue;

		pmap_free_table_tree(level3, 3U);
		(void)pmm_free_page(pmap->root[index] & X86_PTE_ADDRESS);
	}

	(void)pmm_free_page(pmap->root_physical);
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
		!g_x86_vmm.enabled) {
		return false;
	}

	__asm__ volatile("mov %0, %%cr3" : : "r"((uintptr_t)pmap->root_physical) : "memory");

	return true;
}

bool
pmap_deactivate(void)
{
	if (!g_x86_vmm.enabled) return false;

	__asm__ volatile("mov %0, %%cr3" : : "r"((uintptr_t)g_x86_vmm.root_physical) : "memory");

	return true;
}

/* ------------------------------------------------------------------------- */
/* Single-page operations.                                                   */
/* ------------------------------------------------------------------------- */

bool
pmap_enter(pmap_t *pmap, uint64_t va, uint64_t pa, vm_prot_t prot, bool cow, const nxu_cpuset_t *active)
{
	(void)active;

	uint64_t descriptor;

	if (pmap == 0 || pmap->root == 0 || !pmap_user_va_valid(va) || !pmap_user_pa_valid(pa) ||
		!x86_vmm_make_user_descriptor(pa, prot, cow, &descriptor)) {
		return false;
	}

	uint64_t *entry;

	if (!x86_vmm_root_get_l3_entry(pmap->root, va, true, true, &pmap->table_count, &entry)) {
		return false;
	}

	/* Never replace an existing mapping implicitly (use pmap_replace). */
	if ((*entry & X86_PTE_PRESENT) != 0ULL) return false;

	*entry = descriptor;
	pmap_flush_local(); /* harmless if this space is not the active one */

	return true;
}

bool
pmap_remove(pmap_t *pmap, uint64_t va, const nxu_cpuset_t *active)
{
	(void)active;

	if (pmap == 0 || pmap->root == 0 || !pmap_user_va_valid(va)) return false;

	uint64_t *entry;

	if (!x86_vmm_root_get_l3_entry(pmap->root, va, false, true, &pmap->table_count, &entry) ||
		(*entry & X86_PTE_PRESENT) == 0ULL) {
		return false;
	}

	*entry = 0ULL;
	pmap_flush_local();

	return true;
}

bool
pmap_protect(pmap_t *pmap, uint64_t va, vm_prot_t prot, const nxu_cpuset_t *active)
{
	(void)active;

	/* Phase 0/1 narrowing: seal a writable or read-only page read-only and drop
	 * any COW sharing; never widen and never touch an executable page. */
	if (pmap == 0 || pmap->root == 0 || prot != VM_PROT_RO || !pmap_user_va_valid(va)) return false;

	uint64_t *entry;
	vm_prot_t current;

	if (!x86_vmm_root_get_l3_entry(pmap->root, va, false, true, &pmap->table_count, &entry) ||
		!pmap_user_protection(*entry, &current) || current == VM_PROT_RX) {
		return false;
	}

	*entry &= ~(X86_PTE_WRITE | X86_PTE_SOFTWARE_COW);
	pmap_flush_local();

	return true;
}

bool
pmap_replace(pmap_t *pmap, uint64_t va, uint64_t new_pa, vm_prot_t prot, bool cow, const nxu_cpuset_t *active)
{
	(void)active;

	uint64_t descriptor;

	if (pmap == 0 || pmap->root == 0 || !pmap_user_va_valid(va) || !pmap_user_pa_valid(new_pa) ||
		!x86_vmm_make_user_descriptor(new_pa, prot, cow, &descriptor)) {
		return false;
	}

	uint64_t *entry;

	if (!x86_vmm_root_get_l3_entry(pmap->root, va, false, true, &pmap->table_count, &entry) ||
		(*entry & X86_PTE_PRESENT) == 0ULL) {
		return false;
	}

	*entry = descriptor;
	pmap_flush_local();

	return true;
}

/* ------------------------------------------------------------------------- */
/* Read-only inspection.                                                     */
/* ------------------------------------------------------------------------- */

bool
pmap_query(const pmap_t *pmap, uint64_t va, vm_user_page_mapping_t *out)
{
	if (pmap == 0 || pmap->root == 0 || out == 0 || va < X86_USER_NULL_GUARD_SIZE || va >= VM_MAX_USER_ADDRESS) {
		return false;
	}

	uint64_t *entry;
	uint64_t table_count = pmap->table_count;

	if (!x86_vmm_root_get_l3_entry(pmap->root, va, false, true, &table_count, &entry)) return false;

	uint64_t descriptor = *entry;

	if (!pmap_user_protection(descriptor, &out->protection)) return false;

	out->physical_address = descriptor & X86_PTE_ADDRESS;
	out->cow = (descriptor & X86_PTE_SOFTWARE_COW) != 0ULL;

	return true;
}

bool
pmap_translate(const pmap_t *pmap, uint64_t va, bool write, uint64_t *pa)
{
	if (pmap == 0 || pmap->root == 0 || pa == 0 || va < X86_USER_NULL_GUARD_SIZE || va >= VM_MAX_USER_ADDRESS) {
		return false;
	}

	uint64_t *entry;
	uint64_t table_count = pmap->table_count;

	if (!x86_vmm_root_get_l3_entry(pmap->root, va, false, true, &table_count, &entry)) return false;

	uint64_t descriptor = *entry;

	if ((descriptor & (X86_PTE_PRESENT | X86_PTE_USER)) != (X86_PTE_PRESENT | X86_PTE_USER)) return false;

	if (write && (descriptor & X86_PTE_WRITE) == 0ULL) return false;

	*pa = (descriptor & X86_PTE_ADDRESS) | (va & (X86_VM_PAGE_SIZE - 1ULL));

	return true;
}

/* ------------------------------------------------------------------------- */
/* COW / fork helpers.                                                       */
/* ------------------------------------------------------------------------- */

bool
pmap_copy_page(uint64_t dst_pa, uint64_t src_pa)
{
	uint64_t dst_alias;
	uint64_t src_alias;

	if (!pmap_user_pa_valid(dst_pa) || !pmap_user_pa_valid(src_pa) ||
		!vmm_physical_to_higher_half(dst_pa, &dst_alias) || !vmm_physical_to_higher_half(src_pa, &src_alias)) {
		return false;
	}

	memcpy((void *)(uintptr_t)dst_alias, (const void *)(uintptr_t)src_alias, X86_VM_PAGE_SIZE);

	return true;
}

/* Recursively collect present user 4 KiB leaves below one table node. */
static bool
pmap_enumerate_table(uint64_t *table, uint32_t level, uint64_t base, pmap_leaf_t *out, uint64_t cap, uint64_t *count)
{
	static const uint64_t shift[4] = {0ULL, 12ULL, 21ULL, 30ULL};

	for (uint32_t index = 0U; index < X86_USER_TABLE_ENTRIES; index++) {
		uint64_t descriptor = table[index];

		if ((descriptor & X86_PTE_PRESENT) == 0ULL || (descriptor & X86_PTE_LARGE) != 0ULL) continue;

		uint64_t address = base | ((uint64_t)index << shift[level]);

		if (level > 1U) {
			uint64_t *child;

			if (pmap_table_at_entry(descriptor, &child) &&
				!pmap_enumerate_table(child, level - 1U, address, out, cap, count)) {
				return false;
			}

			continue;
		}

		vm_prot_t protection;

		if (!pmap_user_protection(descriptor, &protection) || address >= VM_MAX_USER_ADDRESS) continue;

		if (*count >= cap) return false;

		out[*count].va = address;
		out[*count].pa = descriptor & X86_PTE_ADDRESS;
		out[*count].prot = protection;
		out[*count].cow = (descriptor & X86_PTE_SOFTWARE_COW) != 0ULL;
		(*count)++;
	}

	return true;
}

bool
pmap_enumerate(const pmap_t *pmap, pmap_leaf_t *out, uint64_t cap, uint64_t *count)
{
	if (pmap == 0 || pmap->root == 0 || out == 0 || count == 0) return false;

	uint64_t n = 0ULL;

	for (uint32_t index = 0U; index < X86_KERNEL_FIRST_PML4_INDEX; index++) {
		uint64_t *level3;

		if (!pmap_table_at_entry(pmap->root[index], &level3)) continue;

		if (!pmap_enumerate_table(level3, 3U, (uint64_t)index << 39U, out, cap, &n)) return false;
	}

	*count = n;

	return true;
}

/* Free the present user 4 KiB leaves below one table node via the callback. */
static uint64_t
pmap_release_table(uint64_t *table, uint32_t level, void (*free_page)(uint64_t))
{
	uint64_t released = 0ULL;

	for (uint32_t index = 0U; index < X86_USER_TABLE_ENTRIES; index++) {
		uint64_t descriptor = table[index];

		if ((descriptor & X86_PTE_PRESENT) == 0ULL || (descriptor & X86_PTE_LARGE) != 0ULL) continue;

		if (level == 1U) {
			free_page(descriptor & X86_PTE_ADDRESS);
			table[index] = 0ULL;
			released++;
			continue;
		}

		uint64_t *child;
		if (pmap_table_at_entry(descriptor, &child)) {
			released += pmap_release_table(child, level - 1U, free_page);
		}
	}

	return released;
}

uint64_t
pmap_release_user_pages(pmap_t *pmap, void (*free_page)(uint64_t physical_address))
{
	if (pmap == 0 || pmap->root == 0 || free_page == 0) return 0ULL;

	uint64_t released = 0ULL;

	for (uint32_t index = 0U; index < X86_KERNEL_FIRST_PML4_INDEX; index++) {
		uint64_t *level3;

		if (pmap_table_at_entry(pmap->root[index], &level3)) {
			released += pmap_release_table(level3, 3U, free_page);
		}
	}

	/* One bulk flush for the whole space (teardown). */
	pmap_flush_local();

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
