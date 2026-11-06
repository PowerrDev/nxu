/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/arch/arm64/pmap.c
 *
 * arm64 pmap backend: the architecture-specific half of the user address
 * space, implementing the vm/pmap.h contract against AArch64 stage-1
 * translation tables. All page-table formats, descriptor manipulation, the
 * TTBR0 root, activation and TLB invalidation live here; the neutral layer
 * (vm/address_space.c) owns physical-page ownership, reference counting, COW
 * policy and address-space bookkeeping and reaches translations only through
 * this contract.
 *
 * This backend reuses the shared low-level arm64 table helpers declared in
 * vm/vmm_internal.h (vmm_root_get_l3_entry, vmm_allocate_table, ...), which
 * are also used by the kernel higher-half path (vmm.c, vmm_ttbr1.c). Those
 * remain out of scope for the Phase 0 unification; see doc/vm/unified-pmap.md.
 */

#include <vm/pmap.h>

#include <vm/vmm_internal.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* One past the last byte of the unmapped user null guard (first L3 page). */
#define VM_USER_NULL_GUARD_SIZE VMM_L3_SIZE

/* Every level of the 39-bit walk holds 512 descriptors. */
#define VM_TABLE_ENTRIES 512U

/*
 * AP[2:1] values used by EL0 mappings:
 *   0b01: EL1 read-write, EL0 read-write
 *   0b11: EL1 read-only,  EL0 read-only
 */
#define VM_USER_AP_READ_WRITE (1ULL << 6U)
#define VM_USER_AP_READ_ONLY (3ULL << 6U)

/* PAR_EL1.F is bit zero after an AT instruction. */
#define VM_PAR_FAULT (1ULL << 0U)

/* ------------------------------------------------------------------------- */
/* Descriptor construction and parsing (AArch64 stage-1 leaf format).        */
/* ------------------------------------------------------------------------- */

static bool
pmap_make_user_descriptor(uint64_t physical_address, vm_prot_t protection, bool cow, uint64_t *descriptor)
{
	if (descriptor == 0 || !vmm_physical_page_valid(physical_address)) {
		return false;
	}

	uint64_t attributes = VMM_DESC_ATTR(VMM_MAIR_NORMAL_INDEX) | VMM_DESC_SH_INNER | VMM_DESC_AF | VMM_DESC_PXN;

	switch (protection) {
	case VM_PROT_RW:
		attributes |= VM_USER_AP_READ_WRITE | VMM_DESC_UXN;
		break;

	case VM_PROT_RO:
		attributes |= VM_USER_AP_READ_ONLY | VMM_DESC_UXN;
		break;

	case VM_PROT_RX:
		/*
		 * EL1 may read the page but cannot execute it. EL0 may read and
		 * execute it, while writes are rejected by AP[2:1] = 0b11.
		 */
		attributes |= VM_USER_AP_READ_ONLY;
		break;

	default:
		return false;
	}

	uint64_t value = (physical_address & VMM_DESC_ADDRESS_MASK) | attributes | VMM_DESC_TABLE_PAGE;

	/*
	 * A copy-on-write page is read-only in hardware with the software COW bit
	 * set; force RO regardless of the requested protection so the first write
	 * faults into vm_address_space_cow_break().
	 */
	if (cow) {
		value = (value & ~VMM_DESC_AP_MASK) | VM_USER_AP_READ_ONLY | VMM_DESC_SW_COW;
	}

	*descriptor = value;

	return true;
}

static bool
pmap_descriptor_protection(uint64_t descriptor, vm_prot_t *protection)
{
	if (protection == 0) {
		return false;
	}

	uint64_t access_permissions = descriptor & VMM_DESC_AP_MASK;
	bool privileged_execute_never = (descriptor & VMM_DESC_PXN) != 0ULL;
	bool user_execute_never = (descriptor & VMM_DESC_UXN) != 0ULL;

	if (!privileged_execute_never) {
		return false;
	}

	if (access_permissions == VM_USER_AP_READ_WRITE && user_execute_never) {
		*protection = VM_PROT_RW;
		return true;
	}

	if (access_permissions == VM_USER_AP_READ_ONLY) {
		*protection = user_execute_never ? VM_PROT_RO : VM_PROT_RX;
		return true;
	}

	return false;
}

static bool
pmap_table_from_descriptor(uint64_t descriptor, uint64_t **table)
{
	if ((descriptor & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) return false;

	return vmm_table_pointer_from_physical(descriptor & VMM_DESC_ADDRESS_MASK, table);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle.                                                                */
/* ------------------------------------------------------------------------- */

bool
pmap_create(pmap_t *pmap)
{
	if (pmap == 0 || !g_vmm.enabled || !g_vmm.higher_half_enabled || !g_vmm.higher_half_direct_map_enabled ||
		!g_vmm.kernel_pointers_rebased || pmap->root != 0 || pmap->root_physical != 0ULL || pmap->table_count != 0ULL) {
		return false;
	}

	return vmm_allocate_table(&pmap->root, &pmap->root_physical, &pmap->table_count);
}

bool
pmap_destroy(pmap_t *pmap)
{
	if (pmap == 0 || pmap->root == 0) return false;

	for (uint32_t l1 = 0U; l1 < VM_TABLE_ENTRIES; l1++) {
		uint64_t *level2;
		if (!pmap_table_from_descriptor(pmap->root[l1], &level2)) continue;

		for (uint32_t l2 = 0U; l2 < VM_TABLE_ENTRIES; l2++) {
			if ((level2[l2] & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) continue;

			(void)pmm_free_page(level2[l2] & VMM_DESC_ADDRESS_MASK);
		}

		(void)pmm_free_page(pmap->root[l1] & VMM_DESC_ADDRESS_MASK);
	}

	(void)pmm_free_page(pmap->root_physical);

	memset(pmap, 0, sizeof(*pmap));

	return true;
}

/* ------------------------------------------------------------------------- */
/* Activation (TTBR0_EL1 load / disable).                                    */
/* ------------------------------------------------------------------------- */

bool
pmap_activate(pmap_t *pmap)
{
	if (pmap == 0 || pmap->root == 0 || pmap->root_physical == 0ULL || pmap->table_count == 0ULL || !g_vmm.enabled ||
		!g_vmm.kernel_pointers_rebased) {
		return false;
	}

	uint64_t tcr = vmm_read_tcr() & ~VMM_TCR_EPD0;
	uint64_t ttbr0 = pmap->root_physical & VMM_DESC_ADDRESS_MASK;

	/*
	 * Publish the caller's active-set membership (stored before this call)
	 * ahead of any translation through the new root (activation protocol), then
	 * install the root, enable TTBR0 walks, and drop this CPU's translations of
	 * the previous lower address space. The flush is this CPU's own: only its
	 * TTBR0 changed. Other CPUs' copies of this space are handled when its
	 * tables change (every unmap/permission change invalidates by address on
	 * all CPUs).
	 */
	__asm__ volatile("dsb ish\n"
					 "dsb ishst\n"
					 "msr ttbr0_el1, %0\n"
					 "msr tcr_el1, %1\n"
					 "isb\n"
					 "tlbi vmalle1\n"
					 "dsb nsh\n"
					 "isb\n"
					 :
					 : "r"(ttbr0), "r"(tcr)
					 : "memory");

	return true;
}

bool
pmap_deactivate(void)
{
	uint64_t tcr = vmm_read_tcr() | VMM_TCR_EPD0;

	__asm__ volatile("msr ttbr0_el1, xzr\n"
					 "msr tcr_el1, %0\n"
					 "isb\n"
					 "tlbi vmalle1\n"
					 "dsb nsh\n"
					 "isb\n"
					 :
					 : "r"(tcr)
					 : "memory");

	return true;
}

/* ------------------------------------------------------------------------- */
/* Single-page operations.                                                   */
/* ------------------------------------------------------------------------- */

bool
pmap_enter(pmap_t *pmap, uint64_t va, uint64_t pa, vm_prot_t prot, bool cow, const nxu_cpuset_t *active)
{
	(void)active; /* arm64 installs into an empty slot: no stale entry to shoot down. */

	if (pmap == 0 || pmap->root == 0 || va < VM_USER_NULL_GUARD_SIZE || !vmm_lower_page_valid(va) ||
		!vmm_physical_page_valid(pa)) {
		return false;
	}

	uint64_t descriptor;

	if (!pmap_make_user_descriptor(pa, prot, cow, &descriptor)) return false;

	uint64_t *entry;

	if (!vmm_root_get_l3_entry(pmap->root, va, true, &pmap->table_count, &entry)) {
		return false;
	}

	/* Never replace an existing mapping implicitly (use pmap_replace). */
	if ((*entry & VMM_DESC_VALID) != 0ULL) return false;

	*entry = descriptor;

	/*
	 * Make the new descriptor visible to every CPU's table walker before the
	 * mapping is used. No TLB invalidation: an unmapped page left no entry.
	 */
	vmm_publish_new_mapping();

	return true;
}

bool
pmap_remove(pmap_t *pmap, uint64_t va, const nxu_cpuset_t *active)
{
	(void)active; /* arm64 broadcasts the invalidation to the inner-shareable domain. */

	if (pmap == 0 || pmap->root == 0 || va < VM_USER_NULL_GUARD_SIZE || !vmm_lower_page_valid(va)) {
		return false;
	}

	uint64_t table_count = pmap->table_count;
	uint64_t *entry;

	/* create=false: only ever clears an existing leaf, never allocates, so
	 * table_count is unchanged (the local copy is discarded). */
	if (!vmm_root_get_l3_entry(pmap->root, va, false, &table_count, &entry)) {
		return false;
	}

	if ((*entry & VMM_DESC_VALID) == 0ULL) return false;

	/* Break the mapping, then invalidate its cached translation everywhere:
	 * inner-shareable, by address, whether or not the space is loaded (a thread
	 * of this process may run on any CPU). */
	*entry = 0ULL;
	vmm_invalidate_page(va);

	return true;
}

bool
pmap_protect(pmap_t *pmap, uint64_t va, vm_prot_t prot, const nxu_cpuset_t *active)
{
	(void)active;

	/*
	 * Phase 0 implements the one narrowing the loader needs: make a writable or
	 * read-only page read-only and drop any COW sharing, never granting
	 * permission or touching an executable page. The requested protection must
	 * therefore be read-only.
	 */
	if (pmap == 0 || pmap->root == 0 || prot != VM_PROT_RO || va < VM_USER_NULL_GUARD_SIZE ||
		!vmm_lower_page_valid(va)) {
		return false;
	}

	uint64_t table_count = pmap->table_count;
	uint64_t *entry;

	if (!vmm_root_get_l3_entry(pmap->root, va, false, &table_count, &entry)) return false;

	vm_prot_t current;

	if ((*entry & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE || !pmap_descriptor_protection(*entry, &current) ||
		current == VM_PROT_RX) {
		return false;
	}

	*entry = (*entry & ~(VMM_DESC_AP_MASK | VMM_DESC_SW_COW)) | VM_USER_AP_READ_ONLY;
	vmm_invalidate_page(va);

	return true;
}

bool
pmap_replace(pmap_t *pmap, uint64_t va, uint64_t new_pa, vm_prot_t prot, bool cow, const nxu_cpuset_t *active)
{
	(void)active;

	if (pmap == 0 || pmap->root == 0 || va < VM_USER_NULL_GUARD_SIZE || !vmm_lower_page_valid(va) ||
		!vmm_physical_page_valid(new_pa)) {
		return false;
	}

	uint64_t descriptor;

	if (!pmap_make_user_descriptor(new_pa, prot, cow, &descriptor)) return false;

	uint64_t table_count = pmap->table_count;
	uint64_t *entry;

	if (!vmm_root_get_l3_entry(pmap->root, va, false, &table_count, &entry)) return false;

	/* Only an existing leaf may be replaced. */
	if ((*entry & VMM_DESC_VALID) == 0ULL) return false;

	*entry = descriptor;
	vmm_invalidate_page(va);

	return true;
}

bool
pmap_query(const pmap_t *pmap, uint64_t va, vm_user_page_mapping_t *out)
{
	if (pmap == 0 || pmap->root == 0 || out == 0 || !vmm_lower_page_valid(va)) {
		return false;
	}

	uint64_t table_count = pmap->table_count;
	uint64_t *entry;

	if (!vmm_root_get_l3_entry(pmap->root, va, false, &table_count, &entry)) {
		return false;
	}

	uint64_t descriptor = *entry;

	if ((descriptor & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE ||
		((descriptor & VMM_DESC_ATTR_MASK) >> 2U) != VMM_MAIR_NORMAL_INDEX) {
		return false;
	}

	if (!pmap_descriptor_protection(descriptor, &out->protection)) {
		return false;
	}

	out->physical_address = descriptor & VMM_DESC_ADDRESS_MASK;
	out->cow = (descriptor & VMM_DESC_SW_COW) != 0ULL;

	return true;
}

bool
pmap_translate(const pmap_t *pmap, uint64_t va, bool write, uint64_t *pa)
{
	(void)pmap; /* AT S1E0 translates the EL0 regime the caller has made active. */

	if (pa == 0) return false;

	uint64_t par;

	if (write) {
		__asm__ volatile("at s1e0w, %1\n"
						 "isb\n"
						 "mrs %0, par_el1\n"
						 : "=r"(par)
						 : "r"(va)
						 : "memory");
	} else {
		__asm__ volatile("at s1e0r, %1\n"
						 "isb\n"
						 "mrs %0, par_el1\n"
						 : "=r"(par)
						 : "r"(va)
						 : "memory");
	}

	if ((par & VM_PAR_FAULT) != 0ULL) {
		return false;
	}

	*pa = (par & VMM_DESC_ADDRESS_MASK) | (va & (VMM_L3_SIZE - 1ULL));

	return true;
}

/* ------------------------------------------------------------------------- */
/* COW / fork helpers.                                                       */
/* ------------------------------------------------------------------------- */

bool
pmap_copy_page(uint64_t dst_pa, uint64_t src_pa)
{
	uint64_t dst_kernel;
	uint64_t src_kernel;

	if (!vmm_physical_to_higher_half(src_pa, &src_kernel) || !vmm_physical_to_higher_half(dst_pa, &dst_kernel)) {
		return false;
	}

	memcpy((void *)dst_kernel, (const void *)src_kernel, VMM_L3_SIZE);

	return true;
}

bool
pmap_enumerate(const pmap_t *pmap, pmap_leaf_t *out, uint64_t cap, uint64_t *count)
{
	if (pmap == 0 || pmap->root == 0 || out == 0 || count == 0) return false;

	uint64_t n = 0ULL;

	for (uint32_t l1 = 0U; l1 < VM_TABLE_ENTRIES; l1++) {
		uint64_t *level2;
		if (!pmap_table_from_descriptor(pmap->root[l1], &level2)) continue;

		for (uint32_t l2 = 0U; l2 < VM_TABLE_ENTRIES; l2++) {
			uint64_t *level3;
			if (!pmap_table_from_descriptor(level2[l2], &level3)) continue;

			for (uint32_t l3 = 0U; l3 < VM_TABLE_ENTRIES; l3++) {
				uint64_t descriptor = level3[l3];
				if ((descriptor & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) continue;

				vm_prot_t protection;
				if (!pmap_descriptor_protection(descriptor, &protection)) continue;

				if (n >= cap) return false;

				out[n].va =
					((uint64_t)l1 << VMM_L1_SHIFT) | ((uint64_t)l2 << VMM_L2_SHIFT) | ((uint64_t)l3 << VMM_L3_SHIFT);
				out[n].pa = descriptor & VMM_DESC_ADDRESS_MASK;
				out[n].prot = protection;
				out[n].cow = (descriptor & VMM_DESC_SW_COW) != 0ULL;
				n++;
			}
		}
	}

	*count = n;

	return true;
}

uint64_t
pmap_release_user_pages(pmap_t *pmap, void (*free_page)(uint64_t physical_address))
{
	if (pmap == 0 || pmap->root == 0 || free_page == 0) return 0ULL;

	uint64_t released = 0ULL;

	for (uint32_t l1 = 0U; l1 < VM_TABLE_ENTRIES; l1++) {
		uint64_t *level2;
		if (!pmap_table_from_descriptor(pmap->root[l1], &level2)) continue;

		for (uint32_t l2 = 0U; l2 < VM_TABLE_ENTRIES; l2++) {
			uint64_t *level3;
			if (!pmap_table_from_descriptor(level2[l2], &level3)) continue;

			for (uint32_t l3 = 0U; l3 < VM_TABLE_ENTRIES; l3++) {
				if ((level3[l3] & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) continue;

				free_page(level3[l3] & VMM_DESC_ADDRESS_MASK);
				level3[l3] = 0ULL;
				released++;
			}
		}
	}

	/* One bulk invalidation for the whole space (teardown), inner-shareable. */
	__asm__ volatile("dsb ishst\n"
					 "tlbi vmalle1is\n"
					 "dsb ish\n"
					 "isb\n" ::
						 : "memory");

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
