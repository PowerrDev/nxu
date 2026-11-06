/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/x86_64/address_space.c
 *
 * x86-64 user address spaces built on the shared four-level VMM helpers.
 * This port currently runs one CPU; every user root shares the kernel half.
 */

#include <vm/address_space.h>
#include <vm/x86_64/vmm_internal.h>

#include <kern/machine/machine_routines.h>
#include <kern/machine/vm_param.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define X86_USER_NULL_GUARD_SIZE X86_VM_PAGE_SIZE
#define X86_USER_TABLE_ENTRIES 512U

static vm_address_space_t *g_x86_current_space;

/*
 * Routine:     x86_vm_address_in_shm_window
 * Purpose:
 *              Identify mappings whose pages remain genuinely shared and
 *              writable when a process address space is forked.
 */
static bool
x86_vm_address_in_shm_window(uint64_t address)
{
	return address >= VM_SHM_BASE && address < VM_SHM_BASE + VM_SHM_WINDOW_SIZE;
}

/*
 * Routine:     x86_vm_space_leave
 * Purpose:
 *              Remove the boot CPU from a space after CR3 no longer names it.
 */
static void
x86_vm_space_leave(vm_address_space_t *space)
{
	cpuset_remove_atomic(&space->active_cpus, machine_cpu_id());
	__atomic_store_n(&space->active, !cpuset_empty(&space->active_cpus), __ATOMIC_RELEASE);
}

/*
 * Routine:     x86_vm_page_valid
 * Purpose:
 *              Validate one aligned lower-half page that is not the null page.
 */
static bool
x86_vm_page_valid(uint64_t address)
{
	return address >= X86_USER_NULL_GUARD_SIZE && address < VM_MAX_USER_ADDRESS &&
		(address & (X86_VM_PAGE_SIZE - 1ULL)) == 0ULL;
}

/*
 * Routine:     x86_vm_table_at_entry
 * Purpose:
 *              Resolve a present non-large page-table entry to its direct-map
 *              pointer for recursive address-space operations.
 */
static bool
x86_vm_table_at_entry(uint64_t descriptor, uint64_t **table)
{
	return (descriptor & X86_PTE_PRESENT) != 0ULL && (descriptor & X86_PTE_LARGE) == 0ULL &&
		x86_vmm_table_from_physical(descriptor & X86_PTE_ADDRESS, table);
}

/*
 * Routine:     x86_vm_translate
 * Purpose:
 *              Resolve an active user mapping and enforce the requested read
 *              or write permission before returning its physical address.
 */
static bool
x86_vm_translate(const vm_address_space_t *space, uint64_t address, bool write, uint64_t *physical)
{
	if (!vm_address_space_is_active(space) || physical == 0 || address >= VM_MAX_USER_ADDRESS) {
		return false;
	}

	vm_user_page_mapping_t mapping;
	if (!vm_address_space_query_page(space, address, &mapping) ||
		(write && (mapping.protection != VM_USER_PROTECTION_READ_WRITE || mapping.cow))) {
		return false;
	}

	*physical = mapping.physical_address | (address & (X86_VM_PAGE_SIZE - 1ULL));

	return true;
}

/*
 * Routine:     vm_address_space_current
 * Purpose:
 *              Return the lower-half address space currently loaded in CR3.
 */
const vm_address_space_t *
vm_address_space_current(void)
{
	return g_x86_current_space;
}

/*
 * Routine:     vm_address_space_create
 * Purpose:
 *              Allocate a private PML4 root and copy the shared kernel-half
 *              branches into the new user address space.
 */
bool
vm_address_space_create(vm_address_space_t *space)
{
	if (space == 0 || !g_x86_vmm.enabled || space->pmap.root != 0 || space->pmap.root_physical != 0ULL ||
		space->pmap.table_count != 0ULL || space->active) {
		return false;
	}

	memset(space, 0, sizeof(*space));
	if (!x86_vmm_allocate_table(&space->pmap.root, &space->pmap.root_physical, &space->pmap.table_count)) {
		memset(space, 0, sizeof(*space));
		return false;
	}

	for (uint32_t index = 256U; index < X86_USER_TABLE_ENTRIES; index++) {
		space->pmap.root[index] = g_x86_vmm.root[index];
	}

	return true;
}

/*
 * Routine:     vm_address_space_activate
 * Purpose:
 *              Load one user root into CR3 and track the boot CPU's active
 *              address space for user-copy and fault handling.
 */
bool
vm_address_space_activate(vm_address_space_t *space)
{
	if (space == 0 || space->pmap.root == 0 || space->pmap.root_physical == 0ULL || space->pmap.table_count == 0ULL ||
		!g_x86_vmm.enabled) {
		return false;
	}

	uint64_t irq_state = ml_irq_save();
	vm_address_space_t *previous = g_x86_current_space;

	if (previous == space) {
		ml_irq_restore(irq_state);
		return true;
	}

	cpuset_add_atomic(&space->active_cpus, machine_cpu_id());
	__atomic_store_n(&space->active, true, __ATOMIC_RELEASE);
	__asm__ volatile("mov %0, %%cr3" : : "r"((uintptr_t)space->pmap.root_physical) : "memory");
	g_x86_current_space = space;

	if (previous != 0) {
		x86_vm_space_leave(previous);
	}

	ml_irq_restore(irq_state);

	return true;
}

/*
 * Routine:     vm_address_space_deactivate
 * Purpose:
 *              Return CR3 to the shared kernel root and clear the current
 *              user-space membership for the boot CPU.
 */
bool
vm_address_space_deactivate(void)
{
	if (!g_x86_vmm.enabled) {
		return false;
	}

	uint64_t irq_state = ml_irq_save();
	vm_address_space_t *previous = g_x86_current_space;
	__asm__ volatile("mov %0, %%cr3" : : "r"((uintptr_t)g_x86_vmm.root_physical) : "memory");
	g_x86_current_space = 0;

	if (previous != 0) {
		x86_vm_space_leave(previous);
	}

	ml_irq_restore(irq_state);

	return true;
}

/*
 * Routine:     vm_address_space_quiesce
 * Purpose:
 *              Leave a space on the boot CPU and confirm no CPU retains it
 *              before its page tables or data pages are reclaimed.
 */
bool
vm_address_space_quiesce(vm_address_space_t *space)
{
	if (space == 0 || space->pmap.root == 0) {
		return false;
	}

	if (g_x86_current_space == space && !vm_address_space_deactivate()) {
		return false;
	}

	return cpuset_empty(&space->active_cpus);
}

/*
 * Routine:     vm_address_space_map_page_locked
 * Purpose:
 *              Install one user leaf mapping for a caller that already owns
 *              the address-space lock.
 */
bool
vm_address_space_map_page_locked(vm_address_space_t *space, uint64_t virtual_address, uint64_t physical_address,
								 vm_user_protection_t protection)
{
	uint64_t descriptor;
	if (space == 0 || space->pmap.root == 0 || !x86_vm_page_valid(virtual_address) ||
		!x86_vmm_make_user_descriptor(physical_address, protection, false, &descriptor)) {
		return false;
	}

	uint64_t *entry;
	if (!x86_vmm_root_get_l3_entry(space->pmap.root, virtual_address, true, true, &space->pmap.table_count, &entry) ||
		(*entry & X86_PTE_PRESENT) != 0ULL) {
		return false;
	}

	*entry = descriptor;
	space->resident_pages++;
	if (x86_vm_address_in_shm_window(virtual_address)) {
		space->shared_pages++;
	}

	if (g_x86_current_space == space) {
		x86_vmm_invalidate_page(virtual_address);
	}

	return true;
}

/*
 * Routine:     vm_address_space_map_page
 * Purpose:
 *              Add one read-write, read-only, or executable page to a user
 *              address space, leaving existing mappings unchanged.
 */
bool
vm_address_space_map_page(vm_address_space_t *space, uint64_t virtual_address, uint64_t physical_address,
						  vm_user_protection_t protection)
{
	if (space == 0 || space->pmap.root == 0 || !x86_vm_page_valid(virtual_address)) {
		return false;
	}

	nxu_spin_lock(&space->lock);
	bool mapped = vm_address_space_map_page_locked(space, virtual_address, physical_address, protection);
	nxu_spin_unlock(&space->lock);

	return mapped;
}

/*
 * Routine:     vm_address_space_unmap_page
 * Purpose:
 *              Remove one present user mapping while leaving ownership of its
 *              physical frame with the caller.
 */
bool
vm_address_space_unmap_page(vm_address_space_t *space, uint64_t virtual_address)
{
	if (space == 0 || space->pmap.root == 0 || !x86_vm_page_valid(virtual_address)) {
		return false;
	}

	nxu_spin_lock(&space->lock);
	uint64_t *entry;
	if (!x86_vmm_root_get_l3_entry(space->pmap.root, virtual_address, false, true, &space->pmap.table_count, &entry) ||
		(*entry & X86_PTE_PRESENT) == 0ULL) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	*entry = 0ULL;
	space->resident_pages--;
	if (x86_vm_address_in_shm_window(virtual_address)) {
		space->shared_pages--;
	}

	if (g_x86_current_space == space) {
		x86_vmm_invalidate_page(virtual_address);
	}

	nxu_spin_unlock(&space->lock);

	return true;
}

/*
 * Routine:     vm_address_space_restrict_page
 * Purpose:
 *              Seal an existing non-executable page read-only after loader
 *              relocations have finished.
 */
bool
vm_address_space_restrict_page(vm_address_space_t *space, uint64_t address)
{
	if (space == 0 || space->pmap.root == 0 || !x86_vm_page_valid(address)) {
		return false;
	}

	nxu_spin_lock(&space->lock);
	uint64_t *entry;
	bool valid = x86_vmm_root_get_l3_entry(space->pmap.root, address, false, true, &space->pmap.table_count, &entry) &&
		(*entry & X86_PTE_PRESENT) != 0ULL && (!g_x86_vmm.nx_enabled || (*entry & X86_PTE_NX) != 0ULL);

	if (valid) {
		*entry &= ~(X86_PTE_WRITE | X86_PTE_SOFTWARE_COW);
		if (g_x86_current_space == space) {
			x86_vmm_invalidate_page(address);
		}
	}

	nxu_spin_unlock(&space->lock);

	return valid;
}

/*
 * Routine:     vm_address_space_query_page
 * Purpose:
 *              Read a user mapping's frame, protection, and copy-on-write
 *              state while keeping concurrent table writes serialized.
 */
bool
vm_address_space_query_page(const vm_address_space_t *space, uint64_t virtual_address,
							vm_user_page_mapping_t *mapping)
{
	if (space == 0 || space->pmap.root == 0 || mapping == 0 || virtual_address < X86_USER_NULL_GUARD_SIZE ||
		virtual_address >= VM_MAX_USER_ADDRESS) {
		return false;
	}

	nxu_spinlock_t *lock = (nxu_spinlock_t *)&space->lock;
	nxu_spin_lock(lock);
	uint64_t *entry;
	uint64_t table_count = space->pmap.table_count;
	if (!x86_vmm_root_get_l3_entry(space->pmap.root, virtual_address, false, true, &table_count, &entry) ||
		(*entry & X86_PTE_PRESENT) == 0ULL) {
		nxu_spin_unlock(lock);
		return false;
	}

	uint64_t descriptor = *entry;
	mapping->physical_address = descriptor & X86_PTE_ADDRESS;
	mapping->cow = (descriptor & X86_PTE_SOFTWARE_COW) != 0ULL;

	if (mapping->cow) {
		mapping->protection = VM_USER_PROTECTION_READ_WRITE;
	} else if ((descriptor & X86_PTE_WRITE) != 0ULL) {
		mapping->protection = VM_USER_PROTECTION_READ_WRITE;
	} else if (g_x86_vmm.nx_enabled && (descriptor & X86_PTE_NX) == 0ULL) {
		mapping->protection = VM_USER_PROTECTION_READ_EXECUTE;
	} else {
		mapping->protection = VM_USER_PROTECTION_READ_ONLY;
	}

	nxu_spin_unlock(lock);

	return true;
}

/*
 * Routine:     vm_address_space_translate_read
 * Purpose:
 *              Translate an address in the active user space for a read.
 */
bool
vm_address_space_translate_read(const vm_address_space_t *space, uint64_t virtual_address,
								uint64_t *physical_address)
{
	return x86_vm_translate(space, virtual_address, false, physical_address);
}

/*
 * Routine:     vm_address_space_translate_write
 * Purpose:
 *              Translate an address in the active user space for a write.
 */
bool
vm_address_space_translate_write(const vm_address_space_t *space, uint64_t virtual_address,
								 uint64_t *physical_address)
{
	return x86_vm_translate(space, virtual_address, true, physical_address);
}

/*
 * Routine:     vm_address_space_is_active
 * Purpose:
 *              Report whether the user root is currently loaded by the boot
 *              CPU, rather than merely owned by a task.
 */
bool
vm_address_space_is_active(const vm_address_space_t *space)
{
	return space != 0 && space == g_x86_current_space;
}

/*
 * Routine:     vm_address_space_root_physical
 * Purpose:
 *              Return the CR3-compatible PML4 physical address for a space.
 */
uint64_t
vm_address_space_root_physical(const vm_address_space_t *space)
{
	return space == 0 ? 0ULL : space->pmap.root_physical;
}

/*
 * Routine:     vm_address_space_table_count
 * Purpose:
 *              Return the number of translation-table pages owned by a
 *              user address space.
 */
uint64_t
vm_address_space_table_count(const vm_address_space_t *space)
{
	return space == 0 ? 0ULL : space->pmap.table_count;
}

/*
 * Routine:     x86_vm_release_table_pages
 * Purpose:
 *              Reclaim the mapped data frames below one user page-table node.
 */
static uint64_t
x86_vm_release_table_pages(vm_address_space_t *space, uint64_t *table, uint32_t level)
{
	uint64_t released = 0ULL;

	for (uint32_t index = 0U; index < X86_USER_TABLE_ENTRIES; index++) {
		uint64_t descriptor = table[index];
		if ((descriptor & X86_PTE_PRESENT) == 0ULL) {
			continue;
		}

		if (level == 1U) {
			(void)pmm_free_page(descriptor & X86_PTE_ADDRESS);
			table[index] = 0ULL;
			released++;
			continue;
		}

		if ((descriptor & X86_PTE_LARGE) != 0ULL) {
			continue;
		}

		uint64_t *child;
		if (x86_vm_table_at_entry(descriptor, &child)) {
			released += x86_vm_release_table_pages(space, child, level - 1U);
		}
	}

	(void)space;
	return released;
}

/*
 * Routine:     vm_address_space_release_pages
 * Purpose:
 *              Drop one physical-page owner for every present user mapping
 *              while retaining the page-table hierarchy for later teardown.
 */
uint64_t
vm_address_space_release_pages(vm_address_space_t *space)
{
	if (space == 0 || space->pmap.root == 0) {
		return 0ULL;
	}

	nxu_spin_lock(&space->lock);
	uint64_t released = 0ULL;
	for (uint32_t index = 0U; index < 256U; index++) {
		uint64_t *level3;
		if (x86_vm_table_at_entry(space->pmap.root[index], &level3)) {
			released += x86_vm_release_table_pages(space, level3, 3U);
		}
	}

	space->resident_pages = 0ULL;
	space->shared_pages = 0ULL;
	if (g_x86_current_space == space) {
		uintptr_t root = (uintptr_t)space->pmap.root_physical;
		__asm__ volatile("mov %0, %%cr3" : : "r"(root) : "memory");
	}

	nxu_spin_unlock(&space->lock);

	return released;
}

/*
 * Routine:     x86_vm_free_table_tree
 * Purpose:
 *              Release user-owned intermediate tables without walking into
 *              the shared kernel-half branches.
 */
static void
x86_vm_free_table_tree(uint64_t *table, uint32_t level)
{
	for (uint32_t index = 0U; index < X86_USER_TABLE_ENTRIES; index++) {
		uint64_t descriptor = table[index];
		if ((descriptor & X86_PTE_PRESENT) == 0ULL || level == 1U || (descriptor & X86_PTE_LARGE) != 0ULL) {
			continue;
		}

		uint64_t *child;
		if (x86_vm_table_at_entry(descriptor, &child)) {
			x86_vm_free_table_tree(child, level - 1U);
		}

		(void)pmm_free_page(descriptor & X86_PTE_ADDRESS);
		table[index] = 0ULL;
	}
}

/*
 * Routine:     vm_address_space_destroy
 * Purpose:
 *              Free an inactive address space's user page tables and PML4
 *              root after all mapped data pages have been released.
 */
bool
vm_address_space_destroy(vm_address_space_t *space)
{
	if (space == 0 || space->pmap.root == 0 || space->resident_pages != 0ULL ||
		!vm_address_space_quiesce(space)) {
		return false;
	}

	for (uint32_t index = 0U; index < 256U; index++) {
		uint64_t descriptor = space->pmap.root[index];
		if ((descriptor & X86_PTE_PRESENT) == 0ULL) {
			continue;
		}

		uint64_t *level3;
		if (x86_vm_table_at_entry(descriptor, &level3)) {
			x86_vm_free_table_tree(level3, 3U);
		}

		(void)pmm_free_page(descriptor & X86_PTE_ADDRESS);
	}

	(void)pmm_free_page(space->pmap.root_physical);
	memset(space, 0, sizeof(*space));

	return true;
}

/*
 * Routine:     x86_vm_copy_user_tables
 * Purpose:
 *              Copy present user leaves into a child root, retaining frames
 *              and converting private writable mappings to copy-on-write.
 */
static bool
x86_vm_copy_user_tables(vm_address_space_t *parent, vm_address_space_t *child, uint64_t *source,
						uint32_t level, uint64_t base)
{
	for (uint32_t index = 0U; index < X86_USER_TABLE_ENTRIES; index++) {
		uint64_t descriptor = source[index];
		if ((descriptor & X86_PTE_PRESENT) == 0ULL) {
			continue;
		}

		uint64_t shift = level == 3U ? 30ULL : level == 2U ? 21ULL : level == 1U ? 12ULL : 39ULL;
		uint64_t address = base | ((uint64_t)index << shift);

		if (level > 1U) {
			if ((descriptor & X86_PTE_LARGE) != 0ULL) {
				return false;
			}

			uint64_t *source_child;
			if (!x86_vm_table_at_entry(descriptor, &source_child) ||
				!x86_vm_copy_user_tables(parent, child, source_child, level - 1U, address)) {
				return false;
			}

			continue;
		}

		if (address >= VM_MAX_USER_ADDRESS) {
			continue;
		}

		uint64_t child_descriptor = descriptor;
		bool shared = x86_vm_address_in_shm_window(address);
		if (!shared && (descriptor & X86_PTE_WRITE) != 0ULL) {
			child_descriptor = (descriptor & ~X86_PTE_WRITE) | X86_PTE_SOFTWARE_COW;
		}

		uint64_t *child_entry;
		if (!x86_vmm_root_get_l3_entry(child->pmap.root, address, true, true, &child->pmap.table_count, &child_entry) ||
			(*child_entry & X86_PTE_PRESENT) != 0ULL || !pmm_page_retain(descriptor & X86_PTE_ADDRESS)) {
			return false;
		}

		*child_entry = child_descriptor;
		child->resident_pages++;
		if (shared) {
			child->shared_pages++;
		}

		if (child_descriptor != descriptor) {
			source[index] = child_descriptor;
		}
	}

	(void)parent;
	return true;
}

/*
 * Routine:     vm_address_space_fork
 * Purpose:
 *              Clone user mappings, retain each frame, and mark private
 *              writable pages copy-on-write in both address spaces.
 */
bool
vm_address_space_fork(vm_address_space_t *parent, vm_address_space_t *child)
{
	if (parent == 0 || child == 0 || parent->pmap.root == 0 || !vm_address_space_create(child)) {
		return false;
	}

	nxu_spin_lock(&parent->lock);
	bool copied = true;
	for (uint32_t index = 0U; copied && index < 256U; index++) {
		uint64_t *level3;
		if (!x86_vm_table_at_entry(parent->pmap.root[index], &level3)) {
			continue;
		}

		copied = x86_vm_copy_user_tables(parent, child, level3, 3U, (uint64_t)index << 39U);
	}

	if (g_x86_current_space == parent) {
		uintptr_t root = (uintptr_t)parent->pmap.root_physical;
		__asm__ volatile("mov %0, %%cr3" : : "r"(root) : "memory");
	}

	nxu_spin_unlock(&parent->lock);

	if (!copied) {
		(void)vm_address_space_release_pages(child);
		(void)vm_address_space_destroy(child);
		return false;
	}

	return true;
}

/*
 * Routine:     vm_address_space_cow_break
 * Purpose:
 *              Make a copy-on-write page writable in place when uniquely
 *              owned, or copy it before replacing the current mapping.
 */
bool
vm_address_space_cow_break(vm_address_space_t *space, uint64_t virtual_address)
{
	if (space == 0 || space->pmap.root == 0 || !x86_vm_page_valid(virtual_address)) {
		return false;
	}

	uint64_t page = virtual_address & ~(X86_VM_PAGE_SIZE - 1ULL);
for (uint32_t attempt = 0U; attempt < 8U; attempt++) {
		nxu_spin_lock(&space->lock);
		uint64_t *entry;
		if (!x86_vmm_root_get_l3_entry(space->pmap.root, page, false, true, &space->pmap.table_count, &entry) ||
			(*entry & X86_PTE_PRESENT) == 0ULL) {
			nxu_spin_unlock(&space->lock);
			return false;
		}

		uint64_t original = *entry;
		if ((original & X86_PTE_SOFTWARE_COW) == 0ULL) {
			bool writable = (original & X86_PTE_WRITE) != 0ULL;
			nxu_spin_unlock(&space->lock);
			return writable;
		}

		uint64_t old_physical = original & X86_PTE_ADDRESS;
		if (pmm_page_refcount(old_physical) == 1U) {
			*entry = (original | X86_PTE_WRITE) & ~X86_PTE_SOFTWARE_COW;
			x86_vmm_invalidate_page(page);
			nxu_spin_unlock(&space->lock);
			return true;
		}

		nxu_spin_unlock(&space->lock);

		uint64_t new_physical = 0ULL;
		uint64_t old_alias;
		uint64_t new_alias;
		if (!pmm_allocate_page(&new_physical) || !vmm_physical_to_higher_half(old_physical, &old_alias) ||
			!vmm_physical_to_higher_half(new_physical, &new_alias)) {
			if (new_physical != 0ULL) {
				(void)pmm_free_page(new_physical);
			}
			return false;
		}

		memcpy((void *)(uintptr_t)new_alias, (const void *)(uintptr_t)old_alias, X86_VM_PAGE_SIZE);

		nxu_spin_lock(&space->lock);
		if (!x86_vmm_root_get_l3_entry(space->pmap.root, page, false, true, &space->pmap.table_count, &entry) || *entry != original) {
			nxu_spin_unlock(&space->lock);
			(void)pmm_free_page(new_physical);
			continue;
		}

		*entry = ((original | X86_PTE_WRITE) & ~(X86_PTE_ADDRESS | X86_PTE_SOFTWARE_COW)) |
			(new_physical & X86_PTE_ADDRESS);
		x86_vmm_invalidate_page(page);
		nxu_spin_unlock(&space->lock);
		(void)pmm_free_page(old_physical);
		return true;
	}

	return false;
}
