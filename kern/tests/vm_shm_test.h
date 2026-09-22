/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/vm_shm_test.h
 *
 * Shared-memory-region bootstrap validation.
 */

#ifndef NXU_KERN_TESTS_VM_SHM_TEST_H
#define NXU_KERN_TESTS_VM_SHM_TEST_H

#include <stdbool.h>

bool vm_shm_self_test(void);

/*
 * shm_registry_self_test
 *
 * Repeatedly publishes, attaches, withdraws and releases a region purely
 * through kern/ipc/shm_registry.h's API (no address space, no process --
 * see frameworks/BootDaemons.framework/smptest.c's cases 10/11 for the same
 * proved at EL0 through the syscalls), checking the underlying page's PMM
 * reference count at each step and that pmm_get_free_page_count() is back to
 * where it started once the loop ends.
 */
bool shm_registry_self_test(void);

#endif
