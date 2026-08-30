#include <vm/user_copy.h>

#include <vm/address_space.h>
#include <vm/pmm.h>
#include <vm/vmm.h>

#include <stdint.h>

bool vm_copy_from_user(
	void *destination,
	uint64_t user_address,
	uint64_t size
)
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

		uint64_t chunk_size =
			remaining < remaining_in_page
				? remaining
				: remaining_in_page;

		vm_user_page_mapping_t mapping;

		if (!vm_address_space_query_page(
			space,
			page_address,
			&mapping
		)) {
			return false;
		}

		uint64_t kernel_address;

		if (!vmm_physical_to_higher_half(
			mapping.physical_address,
			&kernel_address
		)) {
			return false;
		}

		const uint8_t *source = (const uint8_t *)(kernel_address + page_offset);

		for (uint64_t index = 0ULL; index < chunk_size; index++) {
			destination_bytes[copied + index] = source[index];
		}

		copied += chunk_size;
	}

	return true;
}

bool vm_copy_to_user(
	uint64_t user_address,
	const void *source,
	uint64_t size
)
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

		uint64_t chunk_size =
			remaining < remaining_in_page
				? remaining
				: remaining_in_page;

		vm_user_page_mapping_t mapping;

		if (!vm_address_space_query_page(
			space,
			page_address,
			&mapping
		)) {
			return false;
		}

		if (
			mapping.protection !=
			VM_USER_PROTECTION_READ_WRITE
		) {
			return false;
		}

		uint64_t kernel_address;

		if (!vmm_physical_to_higher_half(
			mapping.physical_address,
			&kernel_address
		)) {
			return false;
		}

		uint8_t *destination = (uint8_t *)(kernel_address + page_offset);

		for (uint64_t index = 0ULL; index < chunk_size; index++) {
			destination[index] = source_bytes[copied + index];
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
