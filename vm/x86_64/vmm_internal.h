/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/x86_64/vmm_internal.h
 *
 * Private x86-64 page-table helpers shared by VMM and address spaces.
 */
#ifndef NXU_VM_X86_64_VMM_INTERNAL_H
#define NXU_VM_X86_64_VMM_INTERNAL_H

#include <vm/vmm.h>
#include <vm/address_space.h>

#include <stdbool.h>
#include <stdint.h>

#define X86_PTE_PRESENT (1ULL << 0U)
#define X86_PTE_WRITE (1ULL << 1U)
#define X86_PTE_USER (1ULL << 2U)
#define X86_PTE_WRITE_THROUGH (1ULL << 3U)
#define X86_PTE_CACHE_DISABLE (1ULL << 4U)
#define X86_PTE_LARGE (1ULL << 7U)
#define X86_PTE_NX (1ULL << 63U)
#define X86_PTE_ADDRESS 0x000FFFFFFFFFF000ULL
#define X86_PTE_LARGE_ADDRESS 0x000FFFFFFFE00000ULL
#define X86_PTE_SOFTWARE_COW (1ULL << 9U)

#define X86_VM_PAGE_SIZE 4096ULL
#define X86_VM_LARGE_PAGE_SIZE 0x200000ULL
#define X86_VM_INDEX_MASK 0x1FFULL

typedef struct {
	uint64_t *root;
	uint64_t root_physical;
	uint64_t table_count;
	bool enabled;
	bool nx_enabled;
	bool direct_map_enabled;
	bool higher_half_enabled;
	bool pointers_rebased;
} x86_vmm_state_t;

extern x86_vmm_state_t g_x86_vmm;

bool x86_vmm_table_from_physical(uint64_t physical, uint64_t **table);
bool x86_vmm_allocate_table(uint64_t **table, uint64_t *physical, uint64_t *count);
bool x86_vmm_root_get_l3_entry(uint64_t *root, uint64_t address, bool create, bool user,
							   uint64_t *table_count, uint64_t **entry);
bool vmm_lower_page_valid(uint64_t virtual_address);
bool vmm_higher_page_valid(uint64_t virtual_address);
bool vmm_physical_page_valid(uint64_t physical_address);
bool x86_vmm_make_user_descriptor(uint64_t physical, vm_user_protection_t protection, bool cow,
							  uint64_t *descriptor);
void x86_vmm_invalidate_page(uint64_t address);

#endif
