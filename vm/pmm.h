#ifndef NXU_PMM_H
#define NXU_PMM_H

#include <platform/dtb.h>
#include <platform/platform.h>

#include <stdbool.h>
#include <stdint.h>

#define PMM_PAGE_SIZE 4096ULL

typedef struct {
	uint64_t memory_base;
	uint64_t memory_end;

	uint64_t page_count;
	uint64_t free_page_count;
	uint64_t used_page_count;

	uint8_t *bitmap;
	uint64_t bitmap_bytes;
	uint64_t bitmap_storage_bytes;

	uint64_t next_hint;
	bool initialized;
	bool higher_half;
} pmm_state_t;

bool pmm_init(
	const platform_t *platform,
	const dtb_t *dtb
);

/*
 * Ports without a device tree (i386) keep boot-time data out of the
 * allocator by registering its physical range here, before pmm_init(), which
 * reads them alongside the kernel image. Only implemented for those ports:
 * the arm64 pmm learns the same from its dtb.
 */
bool pmm_reserve_boot_range(
	uint64_t base,
	uint64_t size
);

bool pmm_allocate_page(uint64_t *physical_address);
bool pmm_free_page(uint64_t physical_address);

bool pmm_allocate_contiguous_pages(
	uint64_t page_count,
	uint64_t *physical_address
);

bool pmm_free_contiguous_pages(
	uint64_t physical_address,
	uint64_t page_count
);

/*
 * Extra ownership on an already-allocated page, for memory mapped into more
 * than one address space (see vm/vm_shm.h). A freshly allocated page starts
 * with one implicit owner and needs no bookkeeping here; pmm_page_retain
 * records each additional owner beyond that first one, and
 * pmm_free_page/pmm_free_contiguous_pages only actually return a page to
 * the free bitmap once every owner, implicit and retained, has released it.
 */
bool pmm_page_retain(uint64_t physical_address);

/* Total owners of an allocated page (1 with no extra retains, 0 if free/unallocated). */
uint32_t pmm_page_refcount(uint64_t physical_address);

bool pmm_enter_higher_half(void);
bool pmm_higher_half_enabled(void);

uint64_t pmm_get_page_count(void);
uint64_t pmm_get_free_page_count(void);
uint64_t pmm_get_used_page_count(void);

void pmm_dump(void);

#endif
