/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/pmap.h
 *
 * i386 page-table primitives, in the role XNU's pmap plays: the one place
 * that knows the hardware paging format (classic two-level, 4 KiB pages, no
 * PAE, no NX). vmm_i386.c (kernel mappings) and address_space_i386.c (user
 * address spaces) are written on top of it.
 *
 * Structure of the kernel half
 * ----------------------------
 * start.S builds one page directory, i386_boot_pd, out of 4 MiB pages: the
 * direct map of physical [0, VM_DIRECT_MAP_SIZE) at VMM_HIGHER_HALF_BASE.
 * That directory stays the master kernel page directory. pmap_init() refines
 * it in place, once, before any user address space exists:
 *
 *   PDE 768..959   direct map, 4 MiB pages, except the ones covering the
 *                  kernel image, which get 4 KiB page tables so text and
 *                  rodata can be read-only;
 *   PDE 960..975   page tables for the vm_kern arena, pre-allocated;
 *   PDE 976..991   reserved (fixmap), empty;
 *   PDE 992..1023  page tables for the MMIO window, pre-allocated.
 *
 * Every kernel PDE is final after pmap_init(): later kernel mappings only
 * ever fill leaf entries of tables that already exist. That is what lets a
 * new address space copy PDEs 768..1023 once at creation and stay in sync
 * with the kernel forever, with no fault-time fixups.
 *
 * Read-only versus executable: without NX, a "read-execute" page and a
 * "read-only" page are the same hardware mapping. The distinction is kept in
 * a software bit (PTE_SOFTWARE_EXEC, one of the three OS-available bits) so
 * queries report what was mapped.
 */

#ifndef NXU_MACH_I386_PMAP_H
#define NXU_MACH_I386_PMAP_H

#include <mach/i386/multiboot.h>

#include <mach/machine/vm_param.h>

#include <stdbool.h>
#include <stdint.h>

#define PMAP_PAGE_SIZE 0x1000U
#define PMAP_PAGE_MASK 0x0FFFU
#define PMAP_LARGE_PAGE_SIZE 0x00400000U
#define PMAP_TABLE_ENTRIES 1024U

/* Directory entry that maps VMM_HIGHER_HALF_BASE: PDEs below are user space. */
#define PMAP_KERNEL_FIRST_PDE (VMM_HIGHER_HALF_BASE >> 22U)
#define PMAP_DIRECT_MAP_PDES (VM_DIRECT_MAP_SIZE >> 22U)

/* Hardware bits, identical in page-directory and page-table entries. */
#define PTE_PRESENT 0x001U
#define PTE_WRITE 0x002U
#define PTE_USER 0x004U
#define PTE_WRITE_THROUGH 0x008U
#define PTE_CACHE_DISABLE 0x010U
#define PTE_ACCESSED 0x020U
#define PTE_DIRTY 0x040U
#define PTE_LARGE 0x080U /* directory entries only (PS) */
#define PTE_GLOBAL 0x100U

/* Bits 9..11 are ignored by the MMU. */
#define PTE_SOFTWARE_EXEC 0x200U

#define PTE_FRAME 0xFFFFF000U

/* CR0 / CR4 bits the VM layer relies on (start.S sets them). */
#define PMAP_CR0_WP 0x00010000U
#define PMAP_CR0_PG 0x80000000U
#define PMAP_CR4_PSE 0x00000010U

static inline uint32_t pmap_directory_index(uint32_t virtual_address)
{
	return virtual_address >> 22U;
}

static inline uint32_t pmap_table_index(uint32_t virtual_address)
{
	return (virtual_address >> 12U) & 0x3FFU;
}

/* Direct-map pointer to a page-table page (or any page) at a physical address. */
static inline uint32_t *pmap_table_pointer(uint32_t physical_address)
{
	return (uint32_t *)(uintptr_t)(physical_address + VMM_HIGHER_HALF_BASE);
}

/*
 * Early boot, called from start.S with paging on. Rebases the addresses
 * inside the Multiboot structure (command line, memory map, modules) onto the
 * direct map, then removes the temporary identity mapping. Returns the
 * direct-map address of the structure, or 0 if the loader was not Multiboot.
 */
const multiboot_info_t *i386_boot_relocate(uint32_t magic, uint32_t info_physical);

/*
 * Refine the master kernel page directory (see above). Needs the page
 * allocator, so it runs after pmm_init(). Returns false if it already ran or
 * memory is exhausted.
 */
bool pmap_init(void);
bool pmap_initialized(void);

/* The master kernel page directory: pointer, physical address, table pages. */
uint32_t *pmap_kernel_directory(void);
uint32_t pmap_kernel_directory_physical(void);
uint32_t pmap_kernel_table_count(void);

/* CR3 access. pmap_load_directory flushes all non-global TLB entries. */
uint32_t pmap_current_directory_physical(void);
void pmap_load_directory(uint32_t directory_physical);

void pmap_invalidate_page(uint32_t virtual_address);
void pmap_flush_tlb(void);

/* One zeroed page from the page allocator for use as a directory/table. */
bool pmap_table_alloc(uint32_t *physical_address);
void pmap_table_free(uint32_t physical_address);

/*
 * Find the leaf entry for virtual_address in the directory `directory`.
 * With create, a missing page table is allocated (user_tables selects
 * whether its directory entry grants user access) and *table_count is
 * incremented if it is not null. Fails, leaving *entry unset, if the
 * directory entry is absent (and not created) or is a 4 MiB page.
 */
bool pmap_get_entry(
	uint32_t *directory,
	uint32_t virtual_address,
	bool create,
	bool user_tables,
	uint64_t *table_count,
	uint32_t **entry
);

/*
 * Map a physical device range (which may lie above the direct map, e.g. a PCI
 * BAR) uncached into the MMIO window. The returned pointer carries the same
 * offset within its page as `physical_address`. One unmapped guard page
 * follows each mapping.
 */
bool pmap_map_mmio(uint64_t physical_address, uint64_t size, void **virtual_address);
bool pmap_unmap_mmio(void *virtual_address, uint64_t size);
bool pmap_mmio_contains(const void *address);
uint32_t pmap_mmio_pages_in_use(void);

void pmap_dump(void);

#endif
