/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/vm_shm_test.c
 *
 * Kernel-only validation for shared memory regions: a single physical page
 * mapped into two independently created address spaces, written through
 * one, read back through the other, with the underlying page's PMM
 * reference count checked at every step of the map/unmap/release lifecycle.
 */

#include <kern/tests/vm_shm_test.h>

#include <kern/console/console.h>
#include <vm/address_space.h>
#include <vm/pmm.h>
#include <vm/vm_shm.h>

#include <stdbool.h>
#include <stdint.h>

#define VM_SHM_TEST_PATTERN_A 0xA5A5A5A5U
#define VM_SHM_TEST_PATTERN_B 0x5A5A5A5AU

bool vm_shm_self_test(void)
{
	vm_shm_region_t region = VM_SHM_REGION_NULL;
	/* vm_address_space_create() requires a zeroed struct as a precondition. */
	vm_address_space_t space_a = { 0 };
	vm_address_space_t space_b = { 0 };
	uint64_t cursor_a = VM_SHM_BASE;
	uint64_t cursor_b = VM_SHM_BASE;
	uint64_t va_a = 0ULL;
	uint64_t va_b = 0ULL;
	bool mapped_a = false;
	bool mapped_b = false;
	bool passed = false;

	if (!vm_shm_create(PMM_PAGE_SIZE, &region)) goto cleanup;

	uint64_t physical_page = vm_shm_physical_page(region, 0ULL);
	if (physical_page == 0ULL || pmm_page_refcount(physical_page) != 1U) goto cleanup;

	if (!vm_address_space_create(&space_a)) goto cleanup;
	if (!vm_address_space_create(&space_b)) goto cleanup;

	/*
	 * Map into A while nothing is active yet -- the whole point of this
	 * primitive is writing into an address space's tables while some
	 * *other* space (or none) is active, exactly the position WindowServer's
	 * address space is in while an app maps shared memory into it over IPC.
	 */
	if (!vm_shm_map_into(&space_a, &cursor_a, region, VM_USER_PROTECTION_READ_WRITE, &va_a)) goto cleanup;
	mapped_a = true;
	if (pmm_page_refcount(physical_page) != 2U) goto cleanup;

	if (!vm_address_space_activate(&space_a)) goto cleanup;
	*(volatile uint32_t *)va_a = VM_SHM_TEST_PATTERN_A;

	/* Map into B, a second, still-inactive space, while A is active. */
	if (!vm_shm_map_into(&space_b, &cursor_b, region, VM_USER_PROTECTION_READ_WRITE, &va_b)) goto cleanup;
	mapped_b = true;
	if (pmm_page_refcount(physical_page) != 3U) goto cleanup;

	if (!vm_address_space_activate(&space_b)) goto cleanup;
	if (*(volatile uint32_t *)va_b != VM_SHM_TEST_PATTERN_A) goto cleanup;
	*(volatile uint32_t *)va_b = VM_SHM_TEST_PATTERN_B;

	if (!vm_address_space_activate(&space_a)) goto cleanup;
	if (*(volatile uint32_t *)va_a != VM_SHM_TEST_PATTERN_B) goto cleanup;

	if (!vm_shm_unmap_from(&space_b, va_b, region)) goto cleanup;
	mapped_b = false;
	if (pmm_page_refcount(physical_page) != 2U) goto cleanup;

	if (!vm_shm_unmap_from(&space_a, va_a, region)) goto cleanup;
	mapped_a = false;
	if (pmm_page_refcount(physical_page) != 1U) goto cleanup;

	vm_shm_release(region);
	region = VM_SHM_REGION_NULL;
	if (pmm_page_refcount(physical_page) != 0U) goto cleanup;

	passed = true;

cleanup:
	/* Return the CPU to the kernel-only translation regime the rest of
	 * boot expects, regardless of which space (if any) is still active. */
	(void)vm_address_space_deactivate();

	if (mapped_b) (void)vm_shm_unmap_from(&space_b, va_b, region);
	if (mapped_a) (void)vm_shm_unmap_from(&space_a, va_a, region);
	if (region != VM_SHM_REGION_NULL) vm_shm_release(region);

	/*
	 * space_a/space_b's own root (and any intermediate) tables are
	 * intentionally never freed here: vm/address_space.h has no
	 * vm_address_space_destroy yet, matching kern/process/task.c's
	 * task_terminate(), which does not reclaim task->map either -- this is
	 * an existing gap, not one this test introduces. Two throwaway spaces
	 * leaking a small, fixed number of pages once per boot is an accepted
	 * cost until real process exit needs that reclaim built.
	 */

	if (!passed) return false;

	kputln("vm_shm: self-test passed (page shared read/write across two address spaces)");
	return true;
}
