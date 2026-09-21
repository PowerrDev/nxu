#include <vm/address_space.h>
#include <vm/vmm_internal.h>

#include <kern/lock.h>
#include <kern/machine/smp.h>
#include <kern/machine/vm_param.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VM_USER_NULL_GUARD_SIZE VMM_L3_SIZE

/* Every level of the 39-bit walk holds 512 descriptors. */
#define VM_TABLE_ENTRIES 512U

/*
 * AP[2:1] values used by EL0 mappings:
 *
 * 0b01: EL1 read-write, EL0 read-write
 * 0b11: EL1 read-only,  EL0 read-only
 */
#define VM_USER_AP_READ_WRITE (1ULL << 6U)
#define VM_USER_AP_READ_ONLY (3ULL << 6U)

/* PAR_EL1.F is bit zero after an AT instruction. */
#define VM_PAR_FAULT (1ULL << 0U)

/*
 * Lesson 18 begins with one address space and ASID 0.
 *
 * Every activation invalidates the EL1 TLB. ASIDs become useful only once
 * NXU can keep multiple user address spaces alive and switch between them.
 */
static vm_address_space_t *g_active_address_space;

static bool vm_user_descriptor(
	uint64_t physical_address,
	vm_user_protection_t protection,
	uint64_t *descriptor
)
{
	if (
		descriptor == 0 ||
		!vmm_physical_page_valid(physical_address)
	) {
		return false;
	}

	uint64_t attributes =
		VMM_DESC_ATTR(VMM_MAIR_NORMAL_INDEX) |
		VMM_DESC_SH_INNER |
		VMM_DESC_AF |
		VMM_DESC_PXN;

	switch (protection) {
	case VM_USER_PROTECTION_READ_WRITE:
		attributes |=
			VM_USER_AP_READ_WRITE |
			VMM_DESC_UXN;
		break;

	case VM_USER_PROTECTION_READ_ONLY:
		attributes |=
			VM_USER_AP_READ_ONLY |
			VMM_DESC_UXN;
		break;

	case VM_USER_PROTECTION_READ_EXECUTE:
		/*
		 * EL1 may read the page but cannot execute it. EL0 may read and
		 * execute it, while writes are rejected by AP[2:1] = 0b11.
		 */
		attributes |= VM_USER_AP_READ_ONLY;
		break;

	default:
		return false;
	}

	*descriptor =
		(physical_address & VMM_DESC_ADDRESS_MASK) |
		attributes |
		VMM_DESC_TABLE_PAGE;

	return true;
}

static bool vm_user_descriptor_protection(
	uint64_t descriptor,
	vm_user_protection_t *protection
)
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

	if (
		access_permissions == VM_USER_AP_READ_WRITE &&
		user_execute_never
	) {
		*protection = VM_USER_PROTECTION_READ_WRITE;
		return true;
	}

	if (access_permissions == VM_USER_AP_READ_ONLY) {
		*protection = user_execute_never
			? VM_USER_PROTECTION_READ_ONLY
			: VM_USER_PROTECTION_READ_EXECUTE;

		return true;
	}

	return false;
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
		physical_address == 0
	) {
		return false;
	}

	uint64_t par;

	if (write) {
		__asm__ volatile(
			"at s1e0w, %1\n"
			"isb\n"
			"mrs %0, par_el1\n"
			: "=r"(par)
			: "r"(virtual_address)
			: "memory"
		);
	} else {
		__asm__ volatile(
			"at s1e0r, %1\n"
			"isb\n"
			"mrs %0, par_el1\n"
			: "=r"(par)
			: "r"(virtual_address)
			: "memory"
		);
	}

	if ((par & VM_PAR_FAULT) != 0ULL) {
		return false;
	}

	*physical_address =
		(par & VMM_DESC_ADDRESS_MASK) |
		(virtual_address & (VMM_L3_SIZE - 1ULL));

	return true;
}

const vm_address_space_t *vm_address_space_current(void)
{
	return g_active_address_space;
}

bool vm_address_space_create(vm_address_space_t *space)
{
	if (
		space == 0 ||
		!g_vmm.enabled ||
		!g_vmm.higher_half_enabled ||
		!g_vmm.higher_half_direct_map_enabled ||
		!g_vmm.kernel_pointers_rebased ||
		space->root != 0 ||
		space->root_physical != 0ULL ||
		space->table_count != 0ULL ||
		space->active
	) {
		return false;
	}

	memset(space, 0, sizeof(*space));

	if (!vmm_allocate_table(
		&space->root,
		&space->root_physical,
		&space->table_count
	)) {
		memset(space, 0, sizeof(*space));
		return false;
	}

	return true;
}

bool vm_address_space_activate(vm_address_space_t *space)
{
	if (
		space == 0 ||
		space->root == 0 ||
		space->root_physical == 0ULL ||
		space->table_count == 0ULL ||
		!g_vmm.enabled ||
		!g_vmm.kernel_pointers_rebased
	) {
		return false;
	}

	if (
		g_active_address_space == space &&
		!g_vmm.ttbr0_disabled
	) {
		return true;
	}

	/* Preserve table accounting before leaving a previous address space. */
	if (g_active_address_space != 0) {
		g_active_address_space->table_count = g_vmm.table_count;
		g_active_address_space->active = false;
		cpuset_remove_atomic(&g_active_address_space->active_cpus, machine_cpu_id());
	}

	uint64_t tcr = vmm_read_tcr() & ~VMM_TCR_EPD0;
	uint64_t ttbr0 = space->root_physical & VMM_DESC_ADDRESS_MASK;

	/*
	 * TTBR0 was disabled at the end of Lesson 17. Install the new root,
	 * allow TTBR0 walks again, and remove translations from the previous
	 * lower address space before any user mapping can be observed.
	 *
	 * The flush is this CPU's own (not inner-shareable): it is this CPU's
	 * TTBR0 that changed, so only this CPU can hold the previous space's
	 * translations that must go, and a broadcast would also empty every idle
	 * CPU's TLB of kernel (TTBR1) entries on each process switch. What other
	 * CPUs might hold of *this* space is dealt with when its tables change:
	 * every unmap and permission change invalidates by address, inner-shareable
	 * (vmm_invalidate_page), which reaches all CPUs.
	 */
	__asm__ volatile(
		"dsb ishst\n"
		"msr ttbr0_el1, %0\n"
		"msr tcr_el1, %1\n"
		"isb\n"
		"tlbi vmalle1\n"
		"dsb nsh\n"
		"isb\n"
		:
		: "r"(ttbr0), "r"(tcr)
		: "memory"
	);

	g_vmm.root = space->root;
	g_vmm.root_physical = space->root_physical;
	g_vmm.table_count = space->table_count;
	g_vmm.ttbr0_disabled = false;

	space->active = true;
	cpuset_add_atomic(&space->active_cpus, machine_cpu_id());
	g_active_address_space = space;

	return true;
}

bool vm_address_space_deactivate(void)
{
	if (!g_vmm.enabled || !g_vmm.kernel_pointers_rebased) return false;

	if (g_active_address_space != 0) {
		g_active_address_space->table_count = g_vmm.table_count;
		g_active_address_space->active = false;
		cpuset_remove_atomic(&g_active_address_space->active_cpus, machine_cpu_id());
	}

	if (!g_vmm.ttbr0_disabled && !vmm_disable_ttbr0()) return false;

	g_active_address_space = 0;
	return true;
}

bool vm_address_space_map_page(
	vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t physical_address,
	vm_user_protection_t protection
)
{
	if (
		space == 0 ||
		space->root == 0 ||
		virtual_address < VM_USER_NULL_GUARD_SIZE ||
		!vmm_lower_page_valid(virtual_address) ||
		!vmm_physical_page_valid(physical_address)
	) {
		return false;
	}

	uint64_t descriptor;

	if (!vm_user_descriptor(
		physical_address,
		protection,
		&descriptor
	)) {
		return false;
	}

	nxu_spin_lock(&space->lock);

	uint64_t *table_count =
		space == g_active_address_space
			? &g_vmm.table_count
			: &space->table_count;

	uint64_t *entry;

	if (!vmm_root_get_l3_entry(
		space->root,
		virtual_address,
		true,
		table_count,
		&entry
	)) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	if ((*entry & VMM_DESC_VALID) != 0ULL) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	*entry = descriptor;
	vmm_publish_new_mapping();

	if (space == g_active_address_space) {
		space->table_count = g_vmm.table_count;
	}

	nxu_spin_unlock(&space->lock);

	return true;
}

bool vm_address_space_unmap_page(
	vm_address_space_t *space,
	uint64_t virtual_address
)
{
	if (
		space == 0 ||
		space->root == 0 ||
		virtual_address < VM_USER_NULL_GUARD_SIZE ||
		!vmm_lower_page_valid(virtual_address)
	) {
		return false;
	}

	nxu_spin_lock(&space->lock);

	uint64_t table_count =
		space == g_active_address_space
			? g_vmm.table_count
			: space->table_count;

	uint64_t *entry;

	/* create=false: this only ever clears an existing leaf entry, never
	 * allocates intermediate tables, so table_count is never modified here
	 * -- see vm_address_space_query_page for the same read-only usage. */
	if (!vmm_root_get_l3_entry(
		space->root,
		virtual_address,
		false,
		&table_count,
		&entry
	)) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	if ((*entry & VMM_DESC_VALID) == 0ULL) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	/* Break the mapping before invalidating its cached translation, exactly
	 * like vmm_unmap_page does for the kernel TTBR1 side. */
	*entry = 0ULL;

	if (space == g_active_address_space) {
		vmm_invalidate_page(virtual_address);
	} else {
		__asm__ volatile("dsb ish" ::: "memory");
	}

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
		!vmm_lower_page_valid(virtual_address)
	) {
		return false;
	}

	/* space is logically const to callers (this only inspects a mapping),
	 * but the lock still has to be taken to avoid reading a page-table
	 * entry that vm_address_space_map_page/unmap_page is concurrently
	 * publishing on another thread of the same task. */
	nxu_spinlock_t *lock = (nxu_spinlock_t *)&space->lock;

	nxu_spin_lock(lock);

	uint64_t table_count = space->table_count;
	uint64_t *entry;

	if (!vmm_root_get_l3_entry(
		space->root,
		virtual_address,
		false,
		&table_count,
		&entry
	)) {
		nxu_spin_unlock(lock);
		return false;
	}

	uint64_t descriptor = *entry;

	nxu_spin_unlock(lock);

	if (
		(descriptor & VMM_DESC_TYPE_MASK) !=
		VMM_DESC_TABLE_PAGE ||
		((descriptor & VMM_DESC_ATTR_MASK) >> 2U) !=
		VMM_MAIR_NORMAL_INDEX
	) {
		return false;
	}

	if (!vm_user_descriptor_protection(
		descriptor,
		&mapping->protection
	)) {
		return false;
	}

	mapping->physical_address = descriptor & VMM_DESC_ADDRESS_MASK;
	mapping->cow = (descriptor & VMM_DESC_SW_COW) != 0ULL;

	return true;
}

bool vm_address_space_translate_read(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t *physical_address
)
{
	return vm_address_space_translate(
		space,
		virtual_address,
		false,
		physical_address
	);
}

bool vm_address_space_translate_write(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t *physical_address
)
{
	return vm_address_space_translate(
		space,
		virtual_address,
		true,
		physical_address
	);
}

bool vm_address_space_is_active(const vm_address_space_t *space)
{
	return
		space != 0 &&
		space == g_active_address_space &&
		space->active &&
		!g_vmm.ttbr0_disabled;
}

uint64_t vm_address_space_root_physical(
	const vm_address_space_t *space
)
{
	return space == 0 ? 0ULL : space->root_physical;
}

uint64_t vm_address_space_table_count(
	const vm_address_space_t *space
)
{
	if (space == 0) {
		return 0ULL;
	}

	if (space == g_active_address_space) {
		return g_vmm.table_count;
	}

	return space->table_count;
}

/*
 * Page-table walking for fork and teardown. A user address space is a
 * three-level tree (L1 root, L2, L3), every table one page, every user
 * mapping a 4 KiB leaf.
 */

static bool vm_table_from_descriptor(uint64_t descriptor, uint64_t **table)
{
	if ((descriptor & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) return false;

	return vmm_table_pointer_from_physical(descriptor & VMM_DESC_ADDRESS_MASK, table);
}

static bool vm_address_in_shm_window(uint64_t virtual_address)
{
	return
		virtual_address >= VM_SHM_BASE &&
		virtual_address < VM_SHM_BASE + VM_SHM_WINDOW_SIZE;
}

/* Make the CPU forget every cached translation if space is the live one. */
static void vm_address_space_flush_if_active(const vm_address_space_t *space)
{
	if (space != g_active_address_space) {
		__asm__ volatile("dsb ish" ::: "memory");
		return;
	}

	__asm__ volatile(
		"dsb ishst\n"
		"tlbi vmalle1is\n"
		"dsb ish\n"
		"isb\n"
		::: "memory"
	);
}

bool vm_address_space_cow_break(
	vm_address_space_t *space,
	uint64_t virtual_address
)
{
	if (
		space == 0 ||
		space->root == 0 ||
		virtual_address < VM_USER_NULL_GUARD_SIZE ||
		!vmm_lower_page_valid(virtual_address)
	) {
		return false;
	}

	uint64_t page_va = virtual_address & ~(VMM_L3_SIZE - 1ULL);

	nxu_spin_lock(&space->lock);

	uint64_t table_count = space->table_count;
	uint64_t *entry;

	if (!vmm_root_get_l3_entry(space->root, page_va, false, &table_count, &entry)) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	uint64_t descriptor = *entry;

	if (
		(descriptor & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE ||
		(descriptor & VMM_DESC_SW_COW) == 0ULL
	) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	uint64_t old_physical = descriptor & VMM_DESC_ADDRESS_MASK;
	uint64_t writable_attributes =
		(descriptor & ~(VMM_DESC_AP_MASK | VMM_DESC_SW_COW)) |
		VM_USER_AP_READ_WRITE;

	/* The other owner is gone: nothing to copy, just take the page back. */
	if (pmm_page_refcount(old_physical) == 1U) {
		*entry = writable_attributes;

		if (space == g_active_address_space) {
			vmm_invalidate_page(page_va);
		} else {
			__asm__ volatile("dsb ish" ::: "memory");
		}

		nxu_spin_unlock(&space->lock);
		return true;
	}

	nxu_spin_unlock(&space->lock);

	uint64_t new_physical;

	if (!pmm_allocate_page(&new_physical)) return false;

	uint64_t old_kernel;
	uint64_t new_kernel;

	if (
		!vmm_physical_to_higher_half(old_physical, &old_kernel) ||
		!vmm_physical_to_higher_half(new_physical, &new_kernel)
	) {
		(void)pmm_free_page(new_physical);
		return false;
	}

	memcpy((void *)new_kernel, (const void *)old_kernel, VMM_L3_SIZE);

	nxu_spin_lock(&space->lock);

	table_count = space->table_count;

	/* Nothing else runs on this CPU between the two critical sections, but
	 * re-checking keeps the copy from landing on a page that changed. */
	if (
		!vmm_root_get_l3_entry(space->root, page_va, false, &table_count, &entry) ||
		*entry != descriptor
	) {
		nxu_spin_unlock(&space->lock);
		(void)pmm_free_page(new_physical);
		return false;
	}

	*entry = (writable_attributes & ~VMM_DESC_ADDRESS_MASK) | (new_physical & VMM_DESC_ADDRESS_MASK);

	if (space == g_active_address_space) {
		vmm_invalidate_page(page_va);
	} else {
		__asm__ volatile("dsb ish" ::: "memory");
	}

	nxu_spin_unlock(&space->lock);

	/* This space's mapping no longer refers to the shared page. */
	(void)pmm_free_page(old_physical);

	return true;
}

uint64_t vm_address_space_release_pages(vm_address_space_t *space)
{
	if (space == 0 || space->root == 0) return 0ULL;

	uint64_t released = 0ULL;

	nxu_spin_lock(&space->lock);

	for (uint32_t l1 = 0U; l1 < VM_TABLE_ENTRIES; l1++) {
		uint64_t *level2;
		if (!vm_table_from_descriptor(space->root[l1], &level2)) continue;

		for (uint32_t l2 = 0U; l2 < VM_TABLE_ENTRIES; l2++) {
			uint64_t *level3;
			if (!vm_table_from_descriptor(level2[l2], &level3)) continue;

			for (uint32_t l3 = 0U; l3 < VM_TABLE_ENTRIES; l3++) {
				if ((level3[l3] & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) continue;

				(void)pmm_free_page(level3[l3] & VMM_DESC_ADDRESS_MASK);
				level3[l3] = 0ULL;
				released++;
			}
		}
	}

	vm_address_space_flush_if_active(space);

	nxu_spin_unlock(&space->lock);

	return released;
}

bool vm_address_space_destroy(vm_address_space_t *space)
{
	if (space == 0 || space->root == 0) return false;

	/* Never free the tables the CPU is still walking. */
	if (space == g_active_address_space && !vm_address_space_deactivate()) return false;

	for (uint32_t l1 = 0U; l1 < VM_TABLE_ENTRIES; l1++) {
		uint64_t *level2;
		if (!vm_table_from_descriptor(space->root[l1], &level2)) continue;

		for (uint32_t l2 = 0U; l2 < VM_TABLE_ENTRIES; l2++) {
			if ((level2[l2] & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) continue;

			(void)pmm_free_page(level2[l2] & VMM_DESC_ADDRESS_MASK);
		}

		(void)pmm_free_page(space->root[l1] & VMM_DESC_ADDRESS_MASK);
	}

	(void)pmm_free_page(space->root_physical);

	memset(space, 0, sizeof(*space));

	return true;
}

bool vm_address_space_fork(
	vm_address_space_t *parent,
	vm_address_space_t *child
)
{
	if (parent == 0 || child == 0 || parent->root == 0) return false;
	if (!vm_address_space_create(child)) return false;

	bool ok = true;
	bool parent_changed = false;

	nxu_spin_lock(&parent->lock);

	for (uint32_t l1 = 0U; ok && l1 < VM_TABLE_ENTRIES; l1++) {
		uint64_t *level2;
		if (!vm_table_from_descriptor(parent->root[l1], &level2)) continue;

		for (uint32_t l2 = 0U; ok && l2 < VM_TABLE_ENTRIES; l2++) {
			uint64_t *level3;
			if (!vm_table_from_descriptor(level2[l2], &level3)) continue;

			for (uint32_t l3 = 0U; ok && l3 < VM_TABLE_ENTRIES; l3++) {
				uint64_t descriptor = level3[l3];
				if ((descriptor & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) continue;

				uint64_t virtual_address =
					((uint64_t)l1 << VMM_L1_SHIFT) |
					((uint64_t)l2 << VMM_L2_SHIFT) |
					((uint64_t)l3 << VMM_L3_SHIFT);

				uint64_t physical = descriptor & VMM_DESC_ADDRESS_MASK;
				uint64_t child_descriptor = descriptor;

				/* Private writable memory becomes copy-on-write on both
				 * sides; everything else is shared exactly as it is. */
				if (
					!vm_address_in_shm_window(virtual_address) &&
					(descriptor & VMM_DESC_AP_MASK) == VM_USER_AP_READ_WRITE
				) {
					child_descriptor =
						(descriptor & ~VMM_DESC_AP_MASK) |
						VM_USER_AP_READ_ONLY |
						VMM_DESC_SW_COW;
				}

				if (!pmm_page_retain(physical)) {
					ok = false;
					break;
				}

				uint64_t *entry;

				if (!vmm_root_get_l3_entry(
					child->root,
					virtual_address,
					true,
					&child->table_count,
					&entry
				)) {
					(void)pmm_free_page(physical);
					ok = false;
					break;
				}

				*entry = child_descriptor;

				if (child_descriptor != descriptor) {
					level3[l3] = child_descriptor;
					parent_changed = true;
				}
			}
		}
	}

	if (parent_changed) vm_address_space_flush_if_active(parent);

	nxu_spin_unlock(&parent->lock);

	if (!ok) {
		(void)vm_address_space_release_pages(child);
		(void)vm_address_space_destroy(child);
		return false;
	}

	return true;
}
