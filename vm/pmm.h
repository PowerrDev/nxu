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

bool pmm_enter_higher_half(void);
bool pmm_higher_half_enabled(void);

uint64_t pmm_get_page_count(void);
uint64_t pmm_get_free_page_count(void);
uint64_t pmm_get_used_page_count(void);

void pmm_dump(void);

#endif
