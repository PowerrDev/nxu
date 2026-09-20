#include <kern/console/console.h>
#include <kern/memory/heap.h>

#include <vm/pmm.h>
#include <vm/vm_kern.h>
#include <vm/vmm.h>

#include <platform/uart.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HEAP_ALIGNMENT 16ULL
#define HEAP_MIN_SPLIT 16ULL

#define HEAP_PAGE_MAGIC 0x4845415050414745ULL
#define HEAP_BLOCK_MAGIC 0x48454150424C4F43ULL

#define HEAP_MAX_LARGE_ALLOCATIONS 256U

#define HEAP_BLOCK_FREE (1ULL << 0U)

typedef struct heap_page heap_page_t;
typedef struct heap_block heap_block_t;

struct heap_page {
	uint64_t magic;
	heap_page_t *next;

	uint64_t allocation_count;
	uint64_t physical_address;
};

struct heap_block {
	uint64_t magic;
	uint64_t size;

	heap_block_t *next;
	heap_block_t *previous;

	uint64_t flags;
	uint64_t reserved;
};

typedef struct {
	void *address;

	size_t requested_size;
	uint64_t page_count;

	bool active;
} heap_large_allocation_t;

typedef struct {
	heap_page_t *head;

	heap_large_allocation_t
		large_allocations[
			HEAP_MAX_LARGE_ALLOCATIONS
		];

	/*
	 * Number of small arena pages retained by the heap.
	 */
	uint64_t page_count;

	/*
	 * These include both small and large allocations.
	 */
	uint64_t allocation_count;
	uint64_t allocated_bytes;

	uint64_t large_page_count;
	uint64_t large_allocation_count;
	uint64_t large_allocated_bytes;

	bool initialized;
} heap_state_t;

static heap_state_t g_heap;

static bool heap_align_up(
	size_t value,
	size_t alignment,
	size_t *result
)
{
	if (
		result == 0 ||
		alignment == 0U ||
		(alignment & (alignment - 1U)) != 0U
	) {
		return false;
	}

	size_t mask = alignment - 1U;

	if (value > (size_t)-1 - mask) {
		return false;
	}

	*result = (value + mask) & ~mask;

	return true;
}

static bool heap_size_to_pages(
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

	if (pages == 0ULL) {
		return false;
	}

	*page_count = pages;

	return true;
}

static size_t heap_page_header_size(void)
{
	size_t result;

	if (!heap_align_up(
		sizeof(heap_page_t),
		HEAP_ALIGNMENT,
		&result
	)) {
		return 0U;
	}

	return result;
}

static size_t heap_block_header_size(void)
{
	size_t result;

	if (!heap_align_up(
		sizeof(heap_block_t),
		HEAP_ALIGNMENT,
		&result
	)) {
		return 0U;
	}

	return result;
}

static bool heap_block_is_free(
	const heap_block_t *block
)
{
	return (
		block->flags &
		HEAP_BLOCK_FREE
	) != 0ULL;
}

static void heap_block_set_free(
	heap_block_t *block,
	bool free
)
{
	if (free) {
		block->flags |= HEAP_BLOCK_FREE;
	} else {
		block->flags &= ~HEAP_BLOCK_FREE;
	}
}

static void *heap_block_payload(
	heap_block_t *block
)
{
	return
		(uint8_t *)block +
		heap_block_header_size();
}

static heap_block_t *heap_page_first_block(
	heap_page_t *page
)
{
	return (heap_block_t *)(
		(uint8_t *)page +
		heap_page_header_size()
	);
}

static heap_page_t *heap_page_from_address(
	const void *address
)
{
	uint64_t value = (uint64_t)address;

	value &= ~(PMM_PAGE_SIZE - 1ULL);

	return (heap_page_t *)value;
}

static heap_page_t *heap_create_page(void)
{
	uint64_t physical_address;

	if (!pmm_allocate_page(&physical_address)) {
		return 0;
	}

	uint64_t virtual_address;

	if (!vmm_physical_to_higher_half(
		physical_address,
		&virtual_address
	)) {
		(void)pmm_free_page(physical_address);
		return 0;
	}

	heap_page_t *page = (heap_page_t *)virtual_address;

	memset(page, 0, PMM_PAGE_SIZE);

	page->magic = HEAP_PAGE_MAGIC;
	page->next = 0;
	page->allocation_count = 0ULL;
	page->physical_address = physical_address;

	size_t page_header_size = heap_page_header_size();

	size_t block_header_size = heap_block_header_size();

	if (
		page_header_size == 0U ||
		block_header_size == 0U ||
		page_header_size + block_header_size >=
			PMM_PAGE_SIZE
	) {
		(void)pmm_free_page(physical_address);
		return 0;
	}

	heap_block_t *block = heap_page_first_block(page);

	block->magic = HEAP_BLOCK_MAGIC;

	block->size =
		PMM_PAGE_SIZE -
		page_header_size -
		block_header_size;

	block->next = 0;
	block->previous = 0;

	block->flags = HEAP_BLOCK_FREE;
	block->reserved = 0ULL;

	g_heap.page_count++;

	return page;
}

static void heap_append_page(heap_page_t *page)
{
	if (g_heap.head == 0) {
		g_heap.head = page;
		return;
	}

	heap_page_t *current = g_heap.head;

	while (current->next != 0) {
		current = current->next;
	}

	current->next = page;
}

static heap_block_t *heap_find_free_block(
	size_t size,
	heap_page_t **owner
)
{
	for (
		heap_page_t *page = g_heap.head;
		page != 0;
		page = page->next
	) {
		if (page->magic != HEAP_PAGE_MAGIC) {
			return 0;
		}

		for (
			heap_block_t *block = heap_page_first_block(page);
			block != 0;
			block = block->next
		) {
			if (
				block->magic !=
				HEAP_BLOCK_MAGIC
			) {
				return 0;
			}

			if (
				heap_block_is_free(block) &&
				block->size >= size
			) {
				if (owner != 0) {
					*owner = page;
				}

				return block;
			}
		}
	}

	return 0;
}

static void heap_split_block(
	heap_block_t *block,
	size_t requested_size
)
{
	size_t header_size = heap_block_header_size();

	if (
		block->size < requested_size ||
		block->size - requested_size <
			header_size + HEAP_MIN_SPLIT
	) {
		return;
	}

	size_t remaining =
		block->size -
		requested_size -
		header_size;

	heap_block_t *new_block =
		(heap_block_t *)(
			(uint8_t *)heap_block_payload(block) +
			requested_size
		);

	new_block->magic = HEAP_BLOCK_MAGIC;
	new_block->size = remaining;

	new_block->next = block->next;
	new_block->previous = block;

	new_block->flags = HEAP_BLOCK_FREE;
	new_block->reserved = 0ULL;

	if (new_block->next != 0) {
		new_block->next->previous = new_block;
	}

	block->next = new_block;
	block->size = requested_size;
}

static void heap_merge_with_next(
	heap_block_t *block
)
{
	heap_block_t *next = block->next;

	if (
		next == 0 ||
		next->magic != HEAP_BLOCK_MAGIC ||
		!heap_block_is_free(next)
	) {
		return;
	}

	block->size +=
		heap_block_header_size() +
		next->size;

	block->next = next->next;

	if (block->next != 0) {
		block->next->previous = block;
	}

	next->magic = 0ULL;
}

static heap_block_t *heap_coalesce(
	heap_block_t *block
)
{
	heap_merge_with_next(block);

	if (
		block->previous != 0 &&
		heap_block_is_free(block->previous)
	) {
		block = block->previous;
		heap_merge_with_next(block);
	}

	return block;
}

static bool heap_unlink_page(heap_page_t *target)
{
	if (
		target == 0 ||
		target == g_heap.head
	) {
		return false;
	}

	heap_page_t *previous = g_heap.head;

	while (
		previous != 0 &&
		previous->next != target
	) {
		previous = previous->next;
	}

	if (previous == 0) {
		return false;
	}

	heap_page_t *next = target->next;

	previous->next = next;

	uint64_t physical_address = target->physical_address;

	if (!pmm_free_page(physical_address)) {
		/*
		 * Restore the linked list if the PMM rejected the free.
		 */
		previous->next = target;
		return false;
	}

	g_heap.page_count--;

	return true;
}

static heap_large_allocation_t *
heap_find_free_large_record(void)
{
	for (
		uint32_t index = 0U;
		index < HEAP_MAX_LARGE_ALLOCATIONS;
		index++
	) {
		heap_large_allocation_t *record = &g_heap.large_allocations[index];

		if (!record->active) {
			return record;
		}
	}

	return 0;
}

static heap_large_allocation_t *
heap_find_large_record(
	const void *address
)
{
	if (address == 0) {
		return 0;
	}

	for (
		uint32_t index = 0U;
		index < HEAP_MAX_LARGE_ALLOCATIONS;
		index++
	) {
		heap_large_allocation_t *record = &g_heap.large_allocations[index];

		if (
			record->active &&
			record->address == address
		) {
			return record;
		}
	}

	return 0;
}

bool heap_init(void)
{
	if (g_heap.initialized) {
#if defined(__i386__)
		/*
		 * On i386 the VM self-test brings the heap up on its own, so the
		 * kernel phase finding it already running is not an error.
		 */
		return true;
#else
		return false;
#endif
	}

	memset(&g_heap, 0, sizeof(g_heap));

	heap_page_t *page = heap_create_page();

	if (page == 0) {
		return false;
	}

	g_heap.head = page;
	g_heap.initialized = true;

	return true;
}

static void *heap_allocate_large(size_t size)
{
	heap_large_allocation_t *record = heap_find_free_large_record();

	if (record == 0) {
		return 0;
	}

	uint64_t page_count;

	if (!heap_size_to_pages(
		size,
		&page_count
	)) {
		return 0;
	}

	void *address;

	if (!vm_kern_allocate(
		size,
		VMM_PROTECTION_READ_WRITE,
		&address
	)) {
		return 0;
	}

	record->address = address;
	record->requested_size = size;
	record->page_count = page_count;
	record->active = true;

	g_heap.large_page_count += page_count;

	g_heap.large_allocation_count++;

	g_heap.allocation_count++;
	g_heap.allocated_bytes += (uint64_t)size;

	g_heap.large_allocated_bytes += (uint64_t)size;

	return address;
}

static bool heap_free_large(
	heap_large_allocation_t *record
)
{
	if (
		record == 0 ||
		!record->active ||
		record->address == 0
	) {
		return false;
	}

	void *address = record->address;

	size_t requested_size = record->requested_size;

	uint64_t page_count = record->page_count;

	if (
		g_heap.large_page_count <
			page_count ||
		g_heap.large_allocation_count ==
			0ULL ||
		g_heap.allocation_count ==
			0ULL ||
		g_heap.allocated_bytes <
			(uint64_t)requested_size ||
		g_heap.large_allocated_bytes <
			(uint64_t)requested_size
	) {
		return false;
	}

	if (!vm_kern_free(
		address,
		requested_size
	)) {
		return false;
	}

	g_heap.large_page_count -= page_count;

	g_heap.large_allocation_count--;

	g_heap.allocation_count--;
	g_heap.allocated_bytes -= (uint64_t)requested_size;

	g_heap.large_allocated_bytes -= (uint64_t)requested_size;

	memset(record, 0, sizeof(*record));

	return true;
}

void *kmalloc(size_t size)
{
	if (
		!g_heap.initialized ||
		size == 0U
	) {
		return 0;
	}

	size_t aligned_size;

	if (!heap_align_up(
		size,
		HEAP_ALIGNMENT,
		&aligned_size
	)) {
		return 0;
	}

	size_t maximum_size =
		PMM_PAGE_SIZE -
		heap_page_header_size() -
		heap_block_header_size();

	if (aligned_size > maximum_size) {
		return heap_allocate_large(size);
	}	
	
	heap_page_t *page = 0;

	heap_block_t *block =
		heap_find_free_block(
			aligned_size,
			&page
		);

	if (block == 0) {
		page = heap_create_page();

		if (page == 0) {
			return 0;
		}

		heap_append_page(page);

		block = heap_page_first_block(page);
	}

	heap_split_block(
		block,
		aligned_size
	);

	heap_block_set_free(block, false);

	page->allocation_count++;

	g_heap.allocation_count++;
	g_heap.allocated_bytes += block->size;

	return heap_block_payload(block);
}

void *kcalloc(
	size_t count,
	size_t size
)
{
	if (
		count == 0U ||
		size == 0U
	) {
		return 0;
	}

	if (count > (size_t)-1 / size) {
		return 0;
	}

	size_t total_size = count * size;

	void *address = kmalloc(total_size);

	if (address == 0) {
		return 0;
	}

	memset(address, 0, total_size);

	return address;
}

bool kfree(void *address)
{
	if (
		!g_heap.initialized ||
		address == 0
	) {
		return false;
	}

	/*
	 * Large allocations are tracked through separate records.
	 */
	heap_large_allocation_t *large_record = heap_find_large_record(address);

	if (large_record != 0) {
		return heap_free_large(
			large_record
		);
	}

	/*
	 * An address inside vm_kern that does not match an active
	 * large-allocation record is invalid.
	 *
	 * This safely catches:
	 *
	 * - a second free of a large allocation;
	 * - a pointer into the middle of a large allocation;
	 * - an unrelated kernel-arena address.
	 *
	 * The address may already be unmapped, so it must not be
	 * dereferenced as a heap-page header.
	 */
	if (vm_kern_contains(address)) {
		return false;
	}

	/*
	 * Small allocations live in physical pages reached through the
	 * TTBR1 direct map.
	 */
	heap_page_t *page = heap_page_from_address(address);

	if (page->magic != HEAP_PAGE_MAGIC) {
		return false;
	}

	size_t page_header_size = heap_page_header_size();

	size_t block_header_size = heap_block_header_size();

	if (
		page_header_size == 0U ||
		block_header_size == 0U
	) {
		return false;
	}

	uint64_t page_start = (uint64_t)page;

	uint64_t page_end = page_start + PMM_PAGE_SIZE;

	uint64_t payload_address = (uint64_t)address;

	if (
		payload_address <
			page_start +
			page_header_size +
			block_header_size ||
		payload_address >= page_end
	) {
		return false;
	}

	heap_block_t *block =
		(heap_block_t *)(
			(uint8_t *)address -
			block_header_size
		);

	if (
		block->magic != HEAP_BLOCK_MAGIC ||
		heap_block_payload(block) != address ||
		heap_block_is_free(block)
	) {
		return false;
	}

	/*
	 * Prevent accounting underflow if heap metadata has been
	 * corrupted.
	 */
	if (
		g_heap.allocation_count == 0ULL ||
		g_heap.allocated_bytes <
			block->size ||
		page->allocation_count == 0ULL
	) {
		return false;
	}

	g_heap.allocation_count--;

	g_heap.allocated_bytes -= block->size;

	page->allocation_count--;

	heap_block_set_free(block, true);

	(void)heap_coalesce(block);

	/*
	 * Keep the first arena page permanently available so an empty
	 * heap can allocate without immediately requesting another
	 * physical page.
	 */
	if (
		page != g_heap.head &&
		page->allocation_count == 0ULL
	) {
		return heap_unlink_page(page);
	}

	return true;
}

uint64_t heap_get_page_count(void)
{
	return
		g_heap.page_count +
		g_heap.large_page_count;
}

uint64_t heap_get_large_allocation_count(void)
{
	return g_heap.large_allocation_count;
}

uint64_t heap_get_allocation_count(void)
{
	return g_heap.allocation_count;
}

void heap_dump(void)
{
	kprintf("heap: total backing pages: %llu\n", (unsigned long long)(g_heap.page_count + g_heap.large_page_count));
	kprintf("heap: active allocations: %llu\n", (unsigned long long)g_heap.allocation_count);

	if (!kconsole_verbose()) return;

	uint64_t free_blocks = 0ULL;
	uint64_t free_bytes = 0ULL;

	uint64_t used_blocks = 0ULL;
	uint64_t used_bytes = 0ULL;

	for (
		heap_page_t *page = g_heap.head;
		page != 0;
		page = page->next
	) {
		for (
			heap_block_t *block = heap_page_first_block(page);
			block != 0;
			block = block->next
		) {
			if (heap_block_is_free(block)) {
				free_blocks++;
				free_bytes += block->size;
			} else {
				used_blocks++;
				used_bytes += block->size;
			}
		}
	}

	kprintf("heap: small arena pages: %llu\n", (unsigned long long)g_heap.page_count);
	kprintf("heap: large backing pages: %llu\n", (unsigned long long)g_heap.large_page_count);
	kprintf("heap: active large allocations: %llu\n", (unsigned long long)g_heap.large_allocation_count);
	kprintf("heap: total requested bytes: %llu\n", (unsigned long long)g_heap.allocated_bytes);
	kprintf("heap: large requested bytes: %llu\n", (unsigned long long)g_heap.large_allocated_bytes);
	kprintf("heap: small used blocks: %llu\n", (unsigned long long)used_blocks);
	kprintf("heap: small used payload bytes: %llu\n", (unsigned long long)used_bytes);
	kprintf("heap: small free blocks: %llu\n", (unsigned long long)free_blocks);
	kprintf("heap: small free payload bytes: %llu\n", (unsigned long long)free_bytes);
	kprintf("heap: small allocation limit: %llu bytes\n", (unsigned long long)(PMM_PAGE_SIZE - heap_page_header_size() - heap_block_header_size()));
	kprintf("heap: virtual arena limit: %llu bytes\n", (unsigned long long)VM_KERN_SIZE);
	kprintf("heap: large allocation record limit: %llu\n", (unsigned long long)HEAP_MAX_LARGE_ALLOCATIONS);
}
