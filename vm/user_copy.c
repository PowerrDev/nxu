#include <vm/user_copy.h>

#include <kern/lock.h>
#include <vm/address_space.h>
#include <vm/pmap.h>
#include <vm/pmm.h>
#include <vm/vm_fault.h>
#include <vm/vm_kmap.h>

#include <stdint.h>
#include <libk/memops.h>

/*
 * Copy-on-write breaks and munmaps can race with these copies (other threads
 * of the same task run on other CPUs), so each page's lookup and its copy
 * happen under one hold of the space lock: the page cannot be unmapped and
 * freed, nor turned copy-on-write by a fork, between finding it and touching
 * it. The copy goes through the kernel's direct map, which never faults, so
 * nothing below takes the lock again.
 */

/* Attempts per page: a fault resolves the page, but the race it lost to (a
 * fork downgrading it again, an munmap) may need another round. */
#define VM_USER_COPY_ATTEMPTS 4U

/*
 * Copy `size` bytes between `kernel` and the user page at page_address +
 * page_offset if that page is present with the access needed. False when it
 * is not (the caller resolves the fault and tries again).
 */
static bool
vm_user_copy_locked(const vm_address_space_t *space, uint64_t page_address, uint64_t page_offset, void *kernel,
					uint64_t size, bool write)
{
	nxu_spinlock_t *lock = (nxu_spinlock_t *)&space->lock;
	vm_user_page_mapping_t mapping;
	uint64_t kernel_address = 0ULL;

	nxu_spin_lock(lock);

	bool ok = pmap_query(&space->pmap, page_address, &mapping) &&
			  (!write || mapping.protection == VM_USER_PROTECTION_READ_WRITE) &&
			  vm_kmap_phys_to_direct(mapping.physical_address, &kernel_address);

	if (ok) {
		uint8_t *user = (uint8_t *)(uintptr_t)(kernel_address + page_offset);

		if (write)
			nxu_memops_copy_forward(user, kernel, (size_t)size);
		else
			nxu_memops_copy_forward(kernel, user, (size_t)size);
	}

	nxu_spin_unlock(lock);
	return ok;
}

/*
 * One chunk within one page: copy it, first resolving the fault a demand-zero
 * or copy-on-write page would raise -- the kernel reaches user memory through
 * this software walk, so it has to do what the MMU would have asked the fault
 * handler to do.
 */
static bool
vm_user_copy_chunk(const vm_address_space_t *space, uint64_t page_address, uint64_t page_offset, void *kernel,
				   uint64_t size, bool write)
{
	for (uint32_t attempt = 0U; attempt < VM_USER_COPY_ATTEMPTS; attempt++) {
		if (vm_user_copy_locked(space, page_address, page_offset, kernel, size, write)) return true;

		if (!vm_fault_user((vm_address_space_t *)space, page_address, write ? VM_FAULT_WRITE : VM_FAULT_READ))
			return false;
	}

	return false;
}

bool
vm_copy_from_user(void *destination, uint64_t user_address, uint64_t size)
{
	if (size == 0ULL) {
		return true;
	}

	if (destination == 0) {
		return false;
	}

	if (user_address > UINT64_MAX - size) {
		return false;
	}

	const vm_address_space_t *space = vm_address_space_current();

	if (space == 0 || !vm_address_space_is_active(space)) {
		return false;
	}

	uint8_t *destination_bytes = (uint8_t *)destination;

	uint64_t copied = 0ULL;

	while (copied < size) {
		uint64_t current_address = user_address + copied;

		uint64_t page_address = current_address & ~(PMM_PAGE_SIZE - 1ULL);

		uint64_t page_offset = current_address & (PMM_PAGE_SIZE - 1ULL);

		uint64_t remaining_in_page = PMM_PAGE_SIZE - page_offset;

		uint64_t remaining = size - copied;

		uint64_t chunk_size = remaining < remaining_in_page ? remaining : remaining_in_page;

		if (!vm_user_copy_chunk(space, page_address, page_offset, destination_bytes + copied, chunk_size, false)) {
			return false;
		}

		copied += chunk_size;
	}

	return true;
}

bool
vm_copy_to_user(uint64_t user_address, const void *source, uint64_t size)
{
	if (size == 0ULL) {
		return true;
	}

	if (source == 0) {
		return false;
	}

	if (size - 1ULL > UINT64_MAX - user_address) {
		return false;
	}

	const vm_address_space_t *space = vm_address_space_current();

	if (space == 0 || !vm_address_space_is_active(space)) {
		return false;
	}

	const uint8_t *source_bytes = (const uint8_t *)source;

	uint64_t copied = 0ULL;

	while (copied < size) {
		uint64_t current_address = user_address + copied;

		uint64_t page_address = current_address & ~(PMM_PAGE_SIZE - 1ULL);

		uint64_t page_offset = current_address & (PMM_PAGE_SIZE - 1ULL);

		uint64_t remaining_in_page = PMM_PAGE_SIZE - page_offset;

		uint64_t remaining = size - copied;

		uint64_t chunk_size = remaining < remaining_in_page ? remaining : remaining_in_page;

		if (!vm_user_copy_chunk(space, page_address, page_offset, (void *)(uintptr_t)(source_bytes + copied), chunk_size,
								true)) {
			return false;
		}

		copied += chunk_size;
	}

	return true;
}

bool
vm_copy_string_from_user(char *destination, uint64_t user_address, uint64_t capacity)
{
	if (destination == 0 || capacity == 0ULL) return false;

	for (uint64_t index = 0ULL; index < capacity; index++) {
		char character;
		if (index > UINT64_MAX - user_address) return false;
		if (!vm_copy_from_user(&character, user_address + index, 1ULL)) return false;

		destination[index] = character;
		if (character == '\0') return true;
	}

	destination[capacity - 1ULL] = '\0';
	return false;
}
