#include <kern/console/console.h>
#include <vm/vm_kern.h>
#include <vm/pmm.h>
#include <vm/vmm.h>

#include <platform/uart.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VM_KERN_PAGE_COUNT \
	(VM_KERN_SIZE / PMM_PAGE_SIZE)

#define VM_KERN_BITMAP_SIZE \
	((VM_KERN_PAGE_COUNT + 7ULL) / 8ULL)

#define VM_KERN_MAX_ALLOCATIONS 256U

typedef struct {
	uint64_t start_page;
	uint64_t page_count;

	bool active;
} vm_kern_allocation_t;

typedef struct {
	uint8_t bitmap[VM_KERN_BITMAP_SIZE];

	vm_kern_allocation_t
		allocations[VM_KERN_MAX_ALLOCATIONS];

	uint64_t used_pages;
	uint64_t allocation_count;

	bool initialized;
} vm_kern_state_t;

static vm_kern_state_t g_vm_kern;

static bool vm_kern_size_to_pages(
	size_t size,
	uint64_t *page_count
)
{
	if (
		size == 0U ||
		page_count == 0
	) {
		return false;
	}

	uint64_t value = (uint64_t)size;

	uint64_t mask = PMM_PAGE_SIZE - 1ULL;

	if (value > UINT64_MAX - mask) {
		return false;
	}

	uint64_t pages =
		(value + mask) /
		PMM_PAGE_SIZE;

	if (
		pages == 0ULL ||
		pages > VM_KERN_PAGE_COUNT
	) {
		return false;
	}

	*page_count = pages;

	return true;
}

static bool vm_kern_range_valid(
	uint64_t start_page,
	uint64_t page_count
)
{
	if (
		page_count == 0ULL ||
		start_page >= VM_KERN_PAGE_COUNT
	) {
		return false;
	}

	return
		page_count <= VM_KERN_PAGE_COUNT - start_page;
}

static uint64_t vm_kern_page_address(
	uint64_t page_index
)
{
	return
		VM_KERN_BASE +
		page_index * PMM_PAGE_SIZE;
}

static bool vm_kern_page_used(
	uint64_t page_index
)
{
	uint64_t byte_index = page_index >> 3U;

	uint32_t bit_index =
		(uint32_t)(
			page_index & 7ULL
		);

	uint8_t mask = (uint8_t)(1U << bit_index);

	return (
		g_vm_kern.bitmap[byte_index] &
		mask
	) != 0U;
}

static void vm_kern_set_page_used(
	uint64_t page_index,
	bool used
)
{
	uint64_t byte_index = page_index >> 3U;

	uint32_t bit_index =
		(uint32_t)(
			page_index & 7ULL
		);

	uint8_t mask = (uint8_t)(1U << bit_index);

	if (used) {
		g_vm_kern.bitmap[byte_index] |= mask;
	} else {
		g_vm_kern.bitmap[byte_index] &= (uint8_t)~mask;
	}
}

static bool vm_kern_find_free_run(
	uint64_t page_count,
	uint64_t *start_page
)
{
	if (
		page_count == 0ULL ||
		start_page == 0
	) {
		return false;
	}

	uint64_t run_start = 0ULL;
	uint64_t run_length = 0ULL;

	for (
		uint64_t page = 0ULL;
		page < VM_KERN_PAGE_COUNT;
		page++
	) {
		if (vm_kern_page_used(page)) {
			run_length = 0ULL;
			continue;
		}

		if (run_length == 0ULL) {
			run_start = page;
		}

		run_length++;

		if (run_length == page_count) {
			*start_page = run_start;
			return true;
		}
	}

	return false;
}

static bool vm_kern_reserve_run(
	uint64_t start_page,
	uint64_t page_count
)
{
	if (!vm_kern_range_valid(
		start_page,
		page_count
	)) {
		return false;
	}

	for (
		uint64_t index = 0ULL;
		index < page_count;
		index++
	) {
		if (vm_kern_page_used(
			start_page + index
		)) {
			return false;
		}
	}

	for (
		uint64_t index = 0ULL;
		index < page_count;
		index++
	) {
		vm_kern_set_page_used(
			start_page + index,
			true
		);
	}

	g_vm_kern.used_pages += page_count;

	return true;
}

static bool vm_kern_release_run(
	uint64_t start_page,
	uint64_t page_count
)
{
	if (
		!vm_kern_range_valid(
			start_page,
			page_count
		) ||
		g_vm_kern.used_pages < page_count
	) {
		return false;
	}

	for (
		uint64_t index = 0ULL;
		index < page_count;
		index++
	) {
		if (!vm_kern_page_used(
			start_page + index
		)) {
			return false;
		}
	}

	for (
		uint64_t index = 0ULL;
		index < page_count;
		index++
	) {
		vm_kern_set_page_used(
			start_page + index,
			false
		);
	}

	g_vm_kern.used_pages -= page_count;

	return true;
}

static vm_kern_allocation_t *
vm_kern_find_free_record(void)
{
	for (
		uint32_t index = 0U;
		index < VM_KERN_MAX_ALLOCATIONS;
		index++
	) {
		if (
			!g_vm_kern
				.allocations[index]
				.active
		) {
			return
				&g_vm_kern
					.allocations[index];
		}
	}

	return 0;
}

static vm_kern_allocation_t *
vm_kern_find_record(
	uint64_t start_page,
	uint64_t page_count
)
{
	for (
		uint32_t index = 0U;
		index < VM_KERN_MAX_ALLOCATIONS;
		index++
	) {
		vm_kern_allocation_t *record = &g_vm_kern.allocations[index];

		if (
			record->active &&
			record->start_page ==
				start_page &&
			record->page_count ==
				page_count
		) {
			return record;
		}
	}

	return 0;
}

static bool vm_kern_rollback_mappings(
	uint64_t start_page,
	uint64_t mapped_pages
)
{
	while (mapped_pages > 0ULL) {
		mapped_pages--;

		uint64_t virtual_address =
			vm_kern_page_address(
				start_page +
				mapped_pages
			);

		uint64_t physical_address;

		if (!vmm_unmap_page(
			virtual_address,
			&physical_address
		)) {
			return false;
		}

		if (!pmm_free_page(
			physical_address
		)) {
			return false;
		}
	}

	return true;
}

bool vm_kern_init(void)
{
	if (
		g_vm_kern.initialized ||
		!vmm_is_enabled()
	) {
		return false;
	}

	memset(
		&g_vm_kern,
		0,
		sizeof(g_vm_kern)
	);

	g_vm_kern.initialized = true;

	return true;
}

bool vm_kern_allocate(
	size_t size,
	vmm_protection_t protection,
	void **address
)
{
	if (
		!g_vm_kern.initialized ||
		address == 0
	) {
		return false;
	}

	*address = 0;

	uint64_t page_count;

	if (!vm_kern_size_to_pages(
		size,
		&page_count
	)) {
		return false;
	}

	vm_kern_allocation_t *record = vm_kern_find_free_record();

	if (record == 0) {
		return false;
	}

	uint64_t start_page;

	if (!vm_kern_find_free_run(
		page_count,
		&start_page
	)) {
		return false;
	}

	if (!vm_kern_reserve_run(
		start_page,
		page_count
	)) {
		return false;
	}

	uint64_t mapped_pages = 0ULL;

	for (
		uint64_t index = 0ULL;
		index < page_count;
		index++
	) {
		uint64_t physical_address;

		if (!pmm_allocate_page(
			&physical_address
		)) {
			if (vm_kern_rollback_mappings(
				start_page,
				mapped_pages
			)) {
				(void)vm_kern_release_run(
					start_page,
					page_count
				);
			}

			return false;
		}

		uint64_t virtual_address =
			vm_kern_page_address(
				start_page + index
			);

		if (!vmm_map_page(
			virtual_address,
			physical_address,
			VMM_MEMORY_NORMAL,
			protection
		)) {
			(void)pmm_free_page(
				physical_address
			);

			if (vm_kern_rollback_mappings(
				start_page,
				mapped_pages
			)) {
				(void)vm_kern_release_run(
					start_page,
					page_count
				);
			}

			return false;
		}

		mapped_pages++;
	}

	record->start_page = start_page;
	record->page_count = page_count;
	record->active = true;

	g_vm_kern.allocation_count++;

	*address = (void *)
		vm_kern_page_address(start_page);

	return true;
}

bool vm_kern_free(
	void *address,
	size_t size
)
{
	if (
		!g_vm_kern.initialized ||
		address == 0
	) {
		return false;
	}

	uint64_t virtual_address = (uint64_t)address;

	if (
		virtual_address < VM_KERN_BASE ||
		virtual_address >= VM_KERN_END ||
		(
			virtual_address &
			(PMM_PAGE_SIZE - 1ULL)
		) != 0ULL
	) {
		return false;
	}

	uint64_t page_count;

	if (!vm_kern_size_to_pages(
		size,
		&page_count
	)) {
		return false;
	}

	uint64_t start_page =
		(
			virtual_address -
			VM_KERN_BASE
		) /
		PMM_PAGE_SIZE;

	vm_kern_allocation_t *record =
		vm_kern_find_record(
			start_page,
			page_count
		);

	if (record == 0) {
		return false;
	}

	/*
	 * Validate every mapping before changing any page table.
	 */
	for (
		uint64_t index = 0ULL;
		index < page_count;
		index++
	) {
		vmm_page_mapping_t mapping;

		if (!vmm_query_page(
			virtual_address +
				index * PMM_PAGE_SIZE,
			&mapping
		)) {
			return false;
		}

		if (
			mapping.memory_type !=
			VMM_MEMORY_NORMAL
		) {
			return false;
		}
	}

	for (
		uint64_t index = 0ULL;
		index < page_count;
		index++
	) {
		uint64_t page_address =
			virtual_address +
			index * PMM_PAGE_SIZE;

		uint64_t physical_address;

		if (!vmm_unmap_page(
			page_address,
			&physical_address
		)) {
			return false;
		}

		if (!pmm_free_page(
			physical_address
		)) {
			return false;
		}
	}

	if (!vm_kern_release_run(
		start_page,
		page_count
	)) {
		return false;
	}

	memset(record, 0, sizeof(*record));

	g_vm_kern.allocation_count--;

	return true;
}

bool vm_kern_contains(
	const void *address
)
{
	if (address == 0) {
		return false;
	}

	uint64_t value = (uint64_t)address;

	return
		value >= VM_KERN_BASE &&
		value < VM_KERN_END;
}

uint64_t vm_kern_get_total_pages(void)
{
	return VM_KERN_PAGE_COUNT;
}

uint64_t vm_kern_get_used_pages(void)
{
	return g_vm_kern.used_pages;
}

uint64_t vm_kern_get_free_pages(void)
{
	return
		VM_KERN_PAGE_COUNT -
		g_vm_kern.used_pages;
}

uint64_t vm_kern_get_allocation_count(void)
{
	return g_vm_kern.allocation_count;
}

void vm_kern_dump(void)
{
	kputs("vm_kern: range: ");
	kputhex64(VM_KERN_BASE);
	kputc('-');
	kputhex64(VM_KERN_END);
	kputc('\n');

	kputs("vm_kern: virtual size: ");
	kputu64(VM_KERN_SIZE);
	kputln(" bytes");

	kputs("vm_kern: total pages: ");
	kputu64(VM_KERN_PAGE_COUNT);
	kputc('\n');

	kputs("vm_kern: used pages: ");
	kputu64(
		g_vm_kern.used_pages
	);
	kputc('\n');

	kputs("vm_kern: free pages: ");
	kputu64(
		VM_KERN_PAGE_COUNT -
		g_vm_kern.used_pages
	);
	kputc('\n');

	kputs("vm_kern: active allocations: ");
	kputu64(
		g_vm_kern.allocation_count
	);
	kputc('\n');

	kputs("vm_kern: allocation record limit: ");
	kputu64(
		VM_KERN_MAX_ALLOCATIONS
	);
	kputc('\n');
}
