#include <kern/console/console.h>
#include <vm/pmm.h>
#include <vm/vmm.h>
#include <platform/uart.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

static pmm_state_t g_pmm;

static bool pmm_kernel_symbol_physical(
	const void *symbol,
	uint64_t *physical_address
)
{
	return vmm_kernel_address_to_physical(
		(uint64_t)symbol,
		physical_address
	);
}

static bool pmm_pointer_from_physical(
	uint64_t physical_address,
	void **pointer
)
{
	if (pointer == 0) {
		return false;
	}

	if (!g_pmm.higher_half) {
		*pointer = (void *)physical_address;
		return true;
	}

	uint64_t virtual_address;

	if (!vmm_physical_to_higher_half(
		physical_address,
		&virtual_address
	)) {
		return false;
	}

	*pointer = (void *)virtual_address;
	return true;
}

static uint64_t pmm_align_down(
	uint64_t value,
	uint64_t alignment
)
{
	return value & ~(alignment - 1ULL);
}

static bool pmm_align_up(
	uint64_t value,
	uint64_t alignment,
	uint64_t *result
)
{
	if (
		result == 0 ||
		alignment == 0ULL ||
		(alignment & (alignment - 1ULL)) != 0ULL
	) {
		return false;
	}

	uint64_t mask = alignment - 1ULL;

	if (value > UINT64_MAX - mask) {
		return false;
	}

	*result = (value + mask) & ~mask;

	return true;
}

static bool pmm_add(
	uint64_t left,
	uint64_t right,
	uint64_t *result
)
{
	if (result == 0 || left > UINT64_MAX - right) {
		return false;
	}

	*result = left + right;

	return true;
}

static bool pmm_ranges_overlap(
	uint64_t first_start,
	uint64_t first_end,
	uint64_t second_start,
	uint64_t second_end
)
{
	return first_start < second_end &&
		second_start < first_end;
}

static bool pmm_address_to_index(
	uint64_t address,
	uint64_t *index
)
{
	if (
		index == 0 ||
		address < g_pmm.memory_base ||
		address >= g_pmm.memory_end
	) {
		return false;
	}

	*index =
		(address - g_pmm.memory_base) /
		PMM_PAGE_SIZE;

	return true;
}

static bool pmm_bitmap_is_used(uint64_t page_index)
{
	uint64_t byte_index = page_index >> 3U;
	uint8_t bit_mask = (uint8_t)(1U << (page_index & 7U));

	return (
		g_pmm.bitmap[byte_index] &
		bit_mask
	) != 0U;
}

static void pmm_bitmap_set_used(uint64_t page_index)
{
	uint64_t byte_index = page_index >> 3U;
	uint8_t bit_mask = (uint8_t)(1U << (page_index & 7U));

	g_pmm.bitmap[byte_index] |= bit_mask;
}

static void pmm_bitmap_set_free(uint64_t page_index)
{
	uint64_t byte_index = page_index >> 3U;
	uint8_t bit_mask = (uint8_t)(1U << (page_index & 7U));

	g_pmm.bitmap[byte_index] &= (uint8_t)~bit_mask;
}

static void pmm_reserve_page(uint64_t page_index)
{
	if (page_index >= g_pmm.page_count) {
		return;
	}

	if (pmm_bitmap_is_used(page_index)) {
		return;
	}

	pmm_bitmap_set_used(page_index);

	g_pmm.free_page_count--;
	g_pmm.used_page_count++;
}

static void pmm_reserve_range(
	uint64_t start,
	uint64_t size
)
{
	if (size == 0ULL) {
		return;
	}

	uint64_t end;

	if (!pmm_add(start, size, &end)) {
		end = UINT64_MAX;
	}

	if (
		end <= g_pmm.memory_base ||
		start >= g_pmm.memory_end
	) {
		return;
	}

	if (start < g_pmm.memory_base) {
		start = g_pmm.memory_base;
	}

	if (end > g_pmm.memory_end) {
		end = g_pmm.memory_end;
	}

	start = pmm_align_down(start, PMM_PAGE_SIZE);

	if (!pmm_align_up(end, PMM_PAGE_SIZE, &end)) {
		end = g_pmm.memory_end;
	}

	uint64_t first_page =
		(start - g_pmm.memory_base) /
		PMM_PAGE_SIZE;

	uint64_t final_page =
		(end - g_pmm.memory_base) /
		PMM_PAGE_SIZE;

	for (
		uint64_t page = first_page;
		page < final_page;
		page++
	) {
		pmm_reserve_page(page);
	}
}

static bool pmm_find_bitmap_location(
	uint64_t kernel_end,
	uint64_t bitmap_storage_bytes,
	const dtb_t *dtb,
	uint64_t *bitmap_address
)
{
	uint64_t candidate;

	if (!pmm_align_up(
		kernel_end,
		PMM_PAGE_SIZE,
		&candidate
	)) {
		return false;
	}

	uint64_t candidate_end;

	if (!pmm_add(
		candidate,
		bitmap_storage_bytes,
		&candidate_end
	)) {
		return false;
	}

	uint64_t dtb_start = (uint64_t)dtb->base;
	uint64_t dtb_end;

	if (!pmm_add(
		dtb_start,
		dtb->total_size,
		&dtb_end
	)) {
		return false;
	}

	if (pmm_ranges_overlap(
		candidate,
		candidate_end,
		dtb_start,
		dtb_end
	)) {
		if (!pmm_align_up(
			dtb_end,
			PMM_PAGE_SIZE,
			&candidate
		)) {
			return false;
		}

		if (!pmm_add(
			candidate,
			bitmap_storage_bytes,
			&candidate_end
		)) {
			return false;
		}
	}

	if (
		candidate < g_pmm.memory_base ||
		candidate_end > g_pmm.memory_end
	) {
		return false;
	}

	*bitmap_address = candidate;

	return true;
}

bool pmm_init(
	const platform_t *platform,
	const dtb_t *dtb
)
{
	if (
		platform == 0 ||
		dtb == 0 ||
		platform->memory_region_count == 0U
	) {
		return false;
	}

	memset(&g_pmm, 0, sizeof(g_pmm));

	/*
	 * Stage one supports QEMU's single contiguous RAM region.
	 */
	const platform_region_t *memory = &platform->memory_regions[0];

	uint64_t raw_memory_end;

	if (!pmm_add(
		memory->base,
		memory->size,
		&raw_memory_end
	)) {
		return false;
	}

	uint64_t memory_base;

	if (!pmm_align_up(
		memory->base,
		PMM_PAGE_SIZE,
		&memory_base
	)) {
		return false;
	}

	uint64_t memory_end =
		pmm_align_down(
			raw_memory_end,
			PMM_PAGE_SIZE
		);

	if (memory_end <= memory_base) {
		return false;
	}

	g_pmm.memory_base = memory_base;
	g_pmm.memory_end = memory_end;

	g_pmm.page_count =
		(memory_end - memory_base) /
		PMM_PAGE_SIZE;

	g_pmm.bitmap_bytes = (g_pmm.page_count + 7ULL) / 8ULL;

	if (!pmm_align_up(
		g_pmm.bitmap_bytes,
		PMM_PAGE_SIZE,
		&g_pmm.bitmap_storage_bytes
	)) {
		return false;
	}

	uint64_t kernel_start;
	uint64_t kernel_end;

	if (
		!pmm_kernel_symbol_physical(__kernel_start, &kernel_start) ||
		!pmm_kernel_symbol_physical(__kernel_end, &kernel_end)
	) {
		return false;
	}

	uint64_t bitmap_address;

	if (!pmm_find_bitmap_location(
		kernel_end,
		g_pmm.bitmap_storage_bytes,
		dtb,
		&bitmap_address
	)) {
		return false;
	}

	g_pmm.bitmap = (uint8_t *)bitmap_address;

	/*
	 * Zero means free. Every physical page begins as available;
	 * required ranges are reserved immediately afterward.
	 */
	memset(
		g_pmm.bitmap,
		0,
		g_pmm.bitmap_storage_bytes
	);

	g_pmm.free_page_count = g_pmm.page_count;
	g_pmm.used_page_count = 0ULL;

	/*
	 * Keep the boot area and complete kernel image reserved.
	 */
	pmm_reserve_range(
		g_pmm.memory_base,
		kernel_end - g_pmm.memory_base
	);

	pmm_reserve_range(
		bitmap_address,
		g_pmm.bitmap_storage_bytes
	);

	pmm_reserve_range(
		(uint64_t)dtb->base,
		dtb->total_size
	);

	/*
	 * Bits beyond the final real page must never be allocated.
	 */
	uint64_t bitmap_bit_count = g_pmm.bitmap_bytes * 8ULL;

	for (
		uint64_t page = g_pmm.page_count;
		page < bitmap_bit_count;
		page++
	) {
		pmm_bitmap_set_used(page);
	}

	g_pmm.next_hint = 0ULL;
	g_pmm.initialized = true;

	(void)kernel_start;

	return true;
}

bool pmm_allocate_page(uint64_t *physical_address)
{
	if (
		!g_pmm.initialized ||
		physical_address == 0 ||
		g_pmm.free_page_count == 0ULL
	) {
		return false;
	}

	for (
		uint64_t checked = 0;
		checked < g_pmm.page_count;
		checked++
	) {
		uint64_t page_index =
			(g_pmm.next_hint + checked) %
			g_pmm.page_count;

		if (pmm_bitmap_is_used(page_index)) {
			continue;
		}

		pmm_bitmap_set_used(page_index);

		g_pmm.free_page_count--;
		g_pmm.used_page_count++;

		g_pmm.next_hint =
			(page_index + 1ULL) %
			g_pmm.page_count;

		uint64_t address =
			g_pmm.memory_base +
			page_index * PMM_PAGE_SIZE;

		void *page_pointer;

		if (!pmm_pointer_from_physical(address, &page_pointer)) {
			pmm_bitmap_set_free(page_index);
			g_pmm.free_page_count++;
			g_pmm.used_page_count--;
			g_pmm.next_hint = page_index;
			return false;
		}

		memset(page_pointer, 0, PMM_PAGE_SIZE);

		*physical_address = address;

		return true;
	}

	return false;
}

/*
 * pmm_allocate_contiguous_pages:
 *
 * Reserve one physically contiguous run. DMA devices may use the returned
 * base as a single backing segment while the higher-half direct map provides
 * a matching contiguous CPU virtual range.
 */
bool pmm_allocate_contiguous_pages(
	uint64_t page_count,
	uint64_t *physical_address
)
{
	if (
		!g_pmm.initialized ||
		physical_address == 0 ||
		page_count == 0ULL ||
		page_count > g_pmm.free_page_count ||
		page_count > g_pmm.page_count
	) {
		return false;
	}

	for (uint64_t start = 0ULL; start + page_count <= g_pmm.page_count; start++) {
		bool available = true;

		for (uint64_t offset = 0ULL; offset < page_count; offset++) {
			if (!pmm_bitmap_is_used(start + offset)) continue;
			available = false;
			start += offset;
			break;
		}

		if (!available) continue;

		for (uint64_t offset = 0ULL; offset < page_count; offset++) {
			pmm_bitmap_set_used(start + offset);
		}

		g_pmm.free_page_count -= page_count;
		g_pmm.used_page_count += page_count;
		g_pmm.next_hint = (start + page_count) % g_pmm.page_count;

		uint64_t address = g_pmm.memory_base + start * PMM_PAGE_SIZE;
		void *pointer;

		if (!pmm_pointer_from_physical(address, &pointer)) {
			for (uint64_t offset = 0ULL; offset < page_count; offset++) {
				pmm_bitmap_set_free(start + offset);
			}

			g_pmm.free_page_count += page_count;
			g_pmm.used_page_count -= page_count;
			g_pmm.next_hint = start;
			return false;
		}

		if (page_count > UINT64_MAX / PMM_PAGE_SIZE) return false;
		memset(pointer, 0, page_count * PMM_PAGE_SIZE);
		*physical_address = address;
		return true;
	}

	return false;
}

/*
 * pmm_free_contiguous_pages:
 *
 * Release a run previously returned by pmm_allocate_contiguous_pages(). The
 * whole range is validated before the bitmap is changed.
 */
bool pmm_free_contiguous_pages(
	uint64_t physical_address,
	uint64_t page_count
)
{
	if (
		!g_pmm.initialized ||
		page_count == 0ULL ||
		(physical_address & (PMM_PAGE_SIZE - 1ULL)) != 0ULL
	) {
		return false;
	}

	uint64_t first;
	if (!pmm_address_to_index(physical_address, &first)) return false;

	if (page_count > g_pmm.page_count - first) return false;

	for (uint64_t offset = 0ULL; offset < page_count; offset++) {
		if (!pmm_bitmap_is_used(first + offset)) return false;
	}

	for (uint64_t offset = 0ULL; offset < page_count; offset++) {
		pmm_bitmap_set_free(first + offset);
	}

	g_pmm.free_page_count += page_count;
	g_pmm.used_page_count -= page_count;
	if (first < g_pmm.next_hint) g_pmm.next_hint = first;
	return true;
}

bool pmm_free_page(uint64_t physical_address)
{
	if (
		!g_pmm.initialized ||
		(physical_address & (PMM_PAGE_SIZE - 1ULL)) != 0ULL
	) {
		return false;
	}

	uint64_t page_index;

	if (!pmm_address_to_index(
		physical_address,
		&page_index
	)) {
		return false;
	}

	if (!pmm_bitmap_is_used(page_index)) {
		return false;
	}

	pmm_bitmap_set_free(page_index);

	g_pmm.free_page_count++;
	g_pmm.used_page_count--;

	if (page_index < g_pmm.next_hint) {
		g_pmm.next_hint = page_index;
	}

	return true;
}

bool pmm_enter_higher_half(void)
{
	if (
		!g_pmm.initialized ||
		g_pmm.higher_half ||
		!vmm_higher_half_direct_map_enabled()
	) {
		return false;
	}

	uint64_t bitmap_virtual;

	if (!vmm_physical_to_higher_half(
		(uint64_t)g_pmm.bitmap,
		&bitmap_virtual
	)) {
		return false;
	}

	g_pmm.bitmap = (uint8_t *)bitmap_virtual;
	g_pmm.higher_half = true;

	return true;
}

bool pmm_higher_half_enabled(void)
{
	return g_pmm.higher_half;
}

uint64_t pmm_get_page_count(void)
{
	return g_pmm.page_count;
}

uint64_t pmm_get_free_page_count(void)
{
	return g_pmm.free_page_count;
}

uint64_t pmm_get_used_page_count(void)
{
	return g_pmm.used_page_count;
}

void pmm_dump(void)
{
	kputs("pmm: RAM range: ");
	kputhex64(g_pmm.memory_base);
	kputs("-");
	kputhex64(g_pmm.memory_end);
	kputc('\n');

	kputs("pmm: page size: ");
	kputu64(PMM_PAGE_SIZE);
	kputln(" bytes");

	kputs("pmm: total pages: ");
	kputu64(g_pmm.page_count);
	kputc('\n');

	kputs("pmm: used pages: ");
	kputu64(g_pmm.used_page_count);
	kputc('\n');

	kputs("pmm: free pages: ");
	kputu64(g_pmm.free_page_count);
	kputc('\n');

	kputs("pmm: bitmap: ");
	kputhex64((uint64_t)g_pmm.bitmap);
	kputs(", logical size: ");
	kputu64(g_pmm.bitmap_bytes);
	kputs(", reserved size: ");
	kputu64(g_pmm.bitmap_storage_bytes);
	kputln(" bytes");

	kputs("pmm: kernel: ");
	kputhex64((uint64_t)__kernel_start);
	kputs("-");
	kputhex64((uint64_t)__kernel_end);
	kputc('\n');
}
