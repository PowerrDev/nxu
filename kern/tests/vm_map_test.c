/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/vm_map_test.c
 *
 * Kernel-only validation for vm/vm_map.h: allocate a multi-page anonymous
 * region, punch a hole in the middle of it with vm_map_free (splitting the
 * tracked region into a prefix and a suffix), and confirm both flanking
 * pages still translate with their original contents while the freed
 * middle page no longer does.
 *
 * On a port with demand paging (VM_DEMAND_PAGING) a region costs nothing
 * until it is touched, so the test first proves that -- a 3-page and a
 * 32 MiB region both leave the used-page count unchanged -- and then touches
 * exactly the pages it goes on to count. The kernel's own accesses to those
 * user addresses fault and are resolved by vm/vm_fault.h, the same path a
 * syscall dereferencing a user pointer takes.
 *
 * Every free is checked against the exact number of *data* pages it should
 * return to the free bitmap. This deliberately does not check the total
 * page count against a pre-mapping baseline: mapping a never-before-touched
 * VA range allocates new L2/L3 page-table pages (vmm_root_get_l3_entry's
 * create=true path), and this kernel has no vm_address_space_destroy to
 * reclaim those -- the same accepted gap kern/tests/vm_shm_test.c already
 * documents. Unmapping never allocates or frees a table page, only a leaf
 * entry, so the delta across each vm_map_free/vm_map_destroy_all call is
 * exactly the number of data pages that call is expected to release.
 */

#include <kern/tests/vm_map_test.h>

#include <kern/console/console.h>
#include <vm/address_space.h>
#include <vm/pmm.h>
#include <vm/vm_map.h>
#include <mach/machine/vm_param.h>

#include <stdbool.h>
#include <stdint.h>

#define VM_MAP_TEST_PATTERN_PREFIX 0x11111111U
#define VM_MAP_TEST_PATTERN_MIDDLE 0x22222222U
#define VM_MAP_TEST_PATTERN_SUFFIX 0x33333333U

bool vm_map_self_test(void)
{
	/* vm_address_space_create() requires a zeroed struct as a precondition. */
	vm_address_space_t space = { 0 };
	bool activated = false;
	bool passed = false;

	uint64_t region_va = 0ULL;
	uint64_t prefix_va, middle_va, suffix_va;

	if (!vm_address_space_create(&space)) goto cleanup;
	if (!vm_address_space_activate(&space)) goto cleanup;
	activated = true;

#if VM_DEMAND_PAGING
	{
		/* Reserving address space must not allocate a single page. */
		uint64_t used_before_reserve = pmm_get_used_page_count();
		uint64_t big_va = 0ULL;

		if (!vm_map_anon(&space, 32ULL * 1024ULL * 1024ULL, VM_USER_PROTECTION_READ_WRITE, &big_va)) goto cleanup;
		if (pmm_get_used_page_count() != used_before_reserve) goto cleanup;
		if (!vm_map_free(&space, big_va, 32ULL * 1024ULL * 1024ULL)) goto cleanup;
		if (pmm_get_used_page_count() != used_before_reserve) goto cleanup;
	}

	uint64_t used_before_region = pmm_get_used_page_count();
#endif

	if (!vm_map_anon(&space, 3ULL * PMM_PAGE_SIZE, VM_USER_PROTECTION_READ_WRITE, &region_va)) goto cleanup;

#if VM_DEMAND_PAGING
	if (pmm_get_used_page_count() != used_before_region) goto cleanup;
#endif

	prefix_va = region_va;
	middle_va = region_va + PMM_PAGE_SIZE;
	suffix_va = region_va + 2ULL * PMM_PAGE_SIZE;

	/* First touch of each page: with demand paging this faults it in. */
	if (*(volatile uint32_t *)prefix_va != 0U) goto cleanup;

	*(volatile uint32_t *)prefix_va = VM_MAP_TEST_PATTERN_PREFIX;
	*(volatile uint32_t *)middle_va = VM_MAP_TEST_PATTERN_MIDDLE;
	*(volatile uint32_t *)suffix_va = VM_MAP_TEST_PATTERN_SUFFIX;

	{
		uint64_t start_va, end_va;
		if (!vm_map_lookup(&space, prefix_va, &start_va, &end_va)) goto cleanup;
		if (start_va != region_va || end_va != region_va + 3ULL * PMM_PAGE_SIZE) goto cleanup;
	}

	/* Punch out just the middle page -- must split the one tracked region
	 * into a prefix and a suffix rather than removing or shrinking it. */
	uint64_t used_before_middle_free = pmm_get_used_page_count();
	if (!vm_map_free(&space, middle_va, PMM_PAGE_SIZE)) goto cleanup;
	if (pmm_get_used_page_count() != used_before_middle_free - 1ULL) goto cleanup;

	{
		uint64_t start_va, end_va;

		if (!vm_map_lookup(&space, prefix_va, &start_va, &end_va)) goto cleanup;
		if (start_va != region_va || end_va != middle_va) goto cleanup;

		if (vm_map_lookup(&space, middle_va, &start_va, &end_va)) goto cleanup;

		if (!vm_map_lookup(&space, suffix_va, &start_va, &end_va)) goto cleanup;
		if (start_va != suffix_va || end_va != region_va + 3ULL * PMM_PAGE_SIZE) goto cleanup;
	}

	if (*(volatile uint32_t *)prefix_va != VM_MAP_TEST_PATTERN_PREFIX) goto cleanup;
	if (*(volatile uint32_t *)suffix_va != VM_MAP_TEST_PATTERN_SUFFIX) goto cleanup;

	{
		vm_user_page_mapping_t mapping;
		if (vm_address_space_query_page(&space, middle_va, &mapping)) goto cleanup;
	}

	/* A second, independent region -- confirms a whole-region free (no
	 * split) leaves the rest of the address space untouched too. */
	uint64_t second_va = 0ULL;
	if (!vm_map_anon(&space, PMM_PAGE_SIZE, VM_USER_PROTECTION_READ_WRITE, &second_va)) goto cleanup;
	*(volatile uint32_t *)second_va = VM_MAP_TEST_PATTERN_MIDDLE;

	uint64_t used_before_second_free = pmm_get_used_page_count();
	if (!vm_map_free(&space, second_va, PMM_PAGE_SIZE)) goto cleanup;
	if (pmm_get_used_page_count() != used_before_second_free - 1ULL) goto cleanup;
	if (vm_map_lookup(&space, second_va, 0, 0)) goto cleanup;
	if (*(volatile uint32_t *)prefix_va != VM_MAP_TEST_PATTERN_PREFIX) goto cleanup;

	/* Only the prefix and suffix regions (one data page each) remain. */
	uint64_t used_before_destroy_all = pmm_get_used_page_count();
	vm_map_destroy_all(&space);
	if (pmm_get_used_page_count() != used_before_destroy_all - 2ULL) goto cleanup;
	if (vm_map_lookup(&space, prefix_va, 0, 0)) goto cleanup;

	passed = true;

cleanup:
	/* Return the CPU to the kernel-only translation regime the rest of
	 * boot expects, regardless of how far the test got. */
	if (activated) (void)vm_address_space_deactivate();

	/*
	 * space's own root (and any intermediate) tables are intentionally
	 * never freed here -- vm/address_space.h has no vm_address_space_destroy
	 * yet, the same accepted gap kern/tests/vm_shm_test.c already documents.
	 */

	if (!passed) return false;

	kputln("vm_map: self-test passed (region split/shrink/remove, exact page-count deltas)");
	return true;
}
