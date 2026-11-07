/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/vm_kmap.h
 *
 * Neutral kernel-map facade.
 *
 * Architecture-independent VM core code (pmm, dma, user_copy, vm_kern) maps
 * and unmaps kernel pages and converts between physical addresses and their
 * kernel direct-map aliases through this interface, never through the
 * per-architecture VMM (vm/vmm.h), whose names still carry arm64 hardware
 * terms (TTBR0/TTBR1). Every port's VMM already provides the same operations
 * under those names; vm/vm_kmap.c is the single neutral translation unit that
 * binds this facade to the port's VMM, so this header depends on nothing
 * architecture-specific.
 */
#ifndef NXU_VM_KMAP_H
#define NXU_VM_KMAP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	VM_KMAP_PROT_READ_WRITE,
	VM_KMAP_PROT_READ_ONLY,
	VM_KMAP_PROT_READ_EXECUTE,
} vm_kmap_prot_t;

typedef enum {
	VM_KMAP_MEM_NORMAL,
	VM_KMAP_MEM_DEVICE,
} vm_kmap_mem_t;

typedef struct {
	uint64_t physical_address;

	vm_kmap_mem_t memory_type;
	vm_kmap_prot_t protection;
} vm_kmap_mapping_t;

/*
 * Whether stage-1 kernel translation is active.
 */
bool vm_kmap_enabled(void);

/*
 * Install, remove, or inspect one live kernel 4 KiB page mapping. Both
 * addresses must be page-aligned; the virtual address must currently be
 * unmapped for vm_kmap_map_page().
 */
bool vm_kmap_map_page(uint64_t virtual_address, uint64_t physical_address, vm_kmap_mem_t memory_type,
					  vm_kmap_prot_t protection);
bool vm_kmap_unmap_page(uint64_t virtual_address, uint64_t *physical_address);
bool vm_kmap_query_page(uint64_t virtual_address, vm_kmap_mapping_t *mapping);

/*
 * The kernel direct map: a fixed higher-half alias of physical memory.
 */
bool vm_kmap_direct_map_enabled(void);
bool vm_kmap_phys_to_direct(uint64_t physical_address, uint64_t *virtual_address);
bool vm_kmap_direct_to_phys(uint64_t virtual_address, uint64_t *physical_address);

/*
 * Resolve any kernel alias (bootstrap or direct map) of a linked symbol to its
 * physical address.
 */
bool vm_kmap_address_to_phys(uint64_t kernel_address, uint64_t *physical_address);

#endif
