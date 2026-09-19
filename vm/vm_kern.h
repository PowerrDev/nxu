#ifndef NXU_VM_KERN_H
#define NXU_VM_KERN_H

#include <vm/vmm.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <mach/machine/vm_param.h>

#define VM_KERN_END (VM_KERN_BASE + VM_KERN_SIZE)

/*
 * Initialize the kernel virtual-address arena.
 *
 * The VMM must already be initialized and stage-1 translation must
 * already be enabled.
 */
bool vm_kern_init(void);

/*
 * Allocate a virtually contiguous range backed by individual
 * physical pages.
 *
 * The returned address is aligned to PMM_PAGE_SIZE. The requested
 * size is rounded upward to a complete number of pages.
 */
bool vm_kern_allocate(
	size_t size,
	vmm_protection_t protection,
	void **address
);

/*
 * Release a range previously returned by vm_kern_allocate().
 *
 * The address and size must match the original allocation.
 */
bool vm_kern_free(
	void *address,
	size_t size
);

/*
 * Return whether an address is inside the kernel virtual arena.
 */
bool vm_kern_contains(
	const void *address
);

uint64_t vm_kern_get_total_pages(void);
uint64_t vm_kern_get_used_pages(void);
uint64_t vm_kern_get_free_pages(void);
uint64_t vm_kern_get_allocation_count(void);

void vm_kern_dump(void);

#endif
