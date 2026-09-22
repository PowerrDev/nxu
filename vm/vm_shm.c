/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/vm_shm.c
 *
 * Shared-memory region lifecycle. See vm/vm_shm.h for the usage protocol.
 */

#include <vm/vm_shm.h>

#include <kern/lock.h>
#include <kern/memory/heap.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * physical_pages is heap-allocated rather than a fixed-size inline array
 * (region->page_count varies with the caller's requested size) -- the same
 * choice kern/ipc/ipc_kmsg.h makes with ikm_data[] for a variable payload.
 */
struct vm_shm_region {
	uint64_t *physical_pages;
	uint64_t page_count;
	uint64_t size_bytes;
	uint32_t references;
	volatile uint32_t lock;
};

static void vm_shm_lock(vm_shm_region_t region)
{
	while (__atomic_exchange_n(&region->lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
#if defined(__i386__) || defined(__x86_64__)
		__asm__ volatile("pause");
#else
		__asm__ volatile("yield");
#endif
	}
}

static void vm_shm_unlock(vm_shm_region_t region)
{
	__atomic_store_n(&region->lock, 0U, __ATOMIC_RELEASE);
}

bool vm_shm_create(uint64_t size_bytes, vm_shm_region_t *regionp)
{
	if (regionp != 0) *regionp = VM_SHM_REGION_NULL;
	if (regionp == 0 || size_bytes == 0ULL || size_bytes > VM_SHM_MAX_BYTES) return false;

	uint64_t page_count = (size_bytes + PMM_PAGE_SIZE - 1ULL) / PMM_PAGE_SIZE;

	vm_shm_region_t region = kmalloc(sizeof(struct vm_shm_region));
	if (region == 0) return false;

	uint64_t *pages = kmalloc(page_count * sizeof(uint64_t));
	if (pages == 0) {
		(void)kfree(region);
		return false;
	}

	uint64_t allocated = 0ULL;
	for (; allocated < page_count; allocated++) {
		if (!pmm_allocate_page(&pages[allocated])) break;
	}

	if (allocated != page_count) {
		for (uint64_t index = 0ULL; index < allocated; index++) (void)pmm_free_page(pages[index]);
		(void)kfree(pages);
		(void)kfree(region);
		return false;
	}

	region->physical_pages = pages;
	region->page_count = page_count;
	region->size_bytes = size_bytes;
	region->references = 1U;
	region->lock = 0U;

	*regionp = region;
	return true;
}

bool vm_shm_reference(vm_shm_region_t region)
{
	if (region == VM_SHM_REGION_NULL) return false;

	vm_shm_lock(region);
	bool valid = region->references != 0U && region->references != UINT32_MAX;
	if (valid) region->references++;
	vm_shm_unlock(region);
	return valid;
}

void vm_shm_release(vm_shm_region_t region)
{
	if (region == VM_SHM_REGION_NULL) return;

	vm_shm_lock(region);
	bool destroy = false;
	if (region->references != 0U) {
		region->references--;
		destroy = region->references == 0U;
	}
	vm_shm_unlock(region);

	if (!destroy) return;

	/* See the usage protocol in vm/vm_shm.h: any page still mapped
	 * elsewhere stays allocated (pmm_free_page just drops this handle's
	 * own reference), this only actually frees pages nobody unmapped. */
	for (uint64_t index = 0ULL; index < region->page_count; index++) {
		(void)pmm_free_page(region->physical_pages[index]);
	}

	(void)kfree(region->physical_pages);
	(void)kfree(region);
}

uint64_t vm_shm_size(vm_shm_region_t region)
{
	return region == VM_SHM_REGION_NULL ? 0ULL : region->size_bytes;
}

uint64_t vm_shm_page_count(vm_shm_region_t region)
{
	return region == VM_SHM_REGION_NULL ? 0ULL : region->page_count;
}

uint64_t vm_shm_physical_page(vm_shm_region_t region, uint64_t index)
{
	if (region == VM_SHM_REGION_NULL || index >= region->page_count) return 0ULL;
	return region->physical_pages[index];
}

bool vm_shm_map_into(
	vm_address_space_t *space,
	uint64_t *next_va,
	vm_shm_region_t region,
	vm_user_protection_t protection,
	uint64_t *out_va
)
{
	if (out_va != 0) *out_va = 0ULL;
	if (space == 0 || next_va == 0 || region == VM_SHM_REGION_NULL || out_va == 0) return false;

	uint64_t span = region->page_count * PMM_PAGE_SIZE;

	/*
	 * Reserve the range under the space's lock, before anything is mapped: two
	 * threads of a process attaching regions on different CPUs must never be
	 * handed the same base. (A failure below leaves a hole in the window, not an
	 * overlap: the reservation is not given back, since another attach may
	 * already have moved past it.)
	 */
	nxu_spin_lock(&space->lock);

	uint64_t base_va = *next_va;
	if (base_va < VM_SHM_BASE) base_va = VM_SHM_BASE;

	if (span == 0ULL || span > VM_SHM_WINDOW_SIZE || base_va - VM_SHM_BASE > VM_SHM_WINDOW_SIZE - span) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	*next_va = base_va + span;

	nxu_spin_unlock(&space->lock);

	uint64_t mapped = 0ULL;
	for (; mapped < region->page_count; mapped++) {
		uint64_t page_va = base_va + mapped * PMM_PAGE_SIZE;

		if (!pmm_page_retain(region->physical_pages[mapped])) break;

		if (!vm_address_space_map_page(space, page_va, region->physical_pages[mapped], protection)) {
			(void)pmm_free_page(region->physical_pages[mapped]);
			break;
		}
	}

	if (mapped != region->page_count) {
		for (uint64_t index = 0ULL; index < mapped; index++) {
			uint64_t page_va = base_va + index * PMM_PAGE_SIZE;
			(void)vm_address_space_unmap_page(space, page_va);
			(void)pmm_free_page(region->physical_pages[index]);
		}
		return false;
	}

	*out_va = base_va;
	return true;
}

bool vm_shm_unmap_from(
	vm_address_space_t *space,
	uint64_t va,
	vm_shm_region_t region
)
{
	if (space == 0 || region == VM_SHM_REGION_NULL) return false;

	bool all_ok = true;

	for (uint64_t index = 0ULL; index < region->page_count; index++) {
		uint64_t page_va = va + index * PMM_PAGE_SIZE;

		if (!vm_address_space_unmap_page(space, page_va)) {
			all_ok = false;
			continue;
		}

		(void)pmm_free_page(region->physical_pages[index]);
	}

	return all_ok;
}
