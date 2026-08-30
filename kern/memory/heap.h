#ifndef NXU_HEAP_H
#define NXU_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool heap_init(void);

/*
 * Allocate kernel memory.
 *
 * Small allocations use block arenas reached through the TTBR1 direct map.
 * Large allocations use the kernel virtual arena.
 */
void *kmalloc(size_t size);

/*
 * Allocate and zero an array.
 */
void *kcalloc(
	size_t count,
	size_t size
);

/*
 * Release an allocation returned by kmalloc() or kcalloc().
 */
bool kfree(void *address);

/*
 * Return the total number of physical pages currently retained by
 * the heap, including small arenas and large virtual allocations.
 */
uint64_t heap_get_page_count(void);

uint64_t heap_get_allocation_count(void);
uint64_t heap_get_large_allocation_count(void);

void heap_dump(void);

#endif
