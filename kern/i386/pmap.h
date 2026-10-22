/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/pmap.h
 *
 * Declares the kernel i386 pmap interfaces and data structures.
 */
/*
 * PAE paging: four 512-entry directories and a four-entry PDPT. Directory
 * pointers expose a contiguous 2048-entry view, with the last 512 entries
 * shared by every task through final kernel PDEs. CR3 names the PDPT.
 * PAE is required; hardware NX is enabled when CPUID advertises it.
 */

#ifndef NXU_KERN_I386_PMAP_H
#define NXU_KERN_I386_PMAP_H

#include <kern/i386/multiboot.h>

#include <kern/machine/vm_param.h>

#include <stdbool.h>
#include <stdint.h>

#define PMAP_PAGE_SIZE 0x1000U
#define PMAP_PAGE_MASK 0x0FFFU
#define PMAP_LARGE_PAGE_SIZE 0x00200000U
#define PMAP_TABLE_ENTRIES 512U
#define PMAP_DIRECTORY_ENTRIES 2048U
#define PMAP_ROOT_PAGES 5U

/* Directory entry that maps VMM_HIGHER_HALF_BASE: PDEs below are user space. */
#define PMAP_KERNEL_FIRST_PDE (VMM_HIGHER_HALF_BASE >> 21U)
#define PMAP_DIRECT_MAP_PDES (VM_DIRECT_MAP_SIZE >> 21U)

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

#define PTE_FRAME 0xFFFFF000ULL
#define PTE_NX (1ULL << 63U)
#define PMAP_CR4_PAE 0x00000020U
typedef uint64_t pmap_entry_t __attribute__((aligned(8)));

/* Atomic stores prevent torn PAE entries while other CPUs walk shared tables. */
static inline void
pmap_entry_store(pmap_entry_t *entry, pmap_entry_t value)
{
	__atomic_store_n(entry, value, __ATOMIC_SEQ_CST);
}

static inline pmap_entry_t
pmap_entry_load(const pmap_entry_t *entry)
{
	return __atomic_load_n(entry, __ATOMIC_SEQ_CST);
}

bool pmap_nx_enabled(void);
bool pmap_cpu_protection_valid(void);
pmap_entry_t pmap_data_flags(void);
pmap_entry_t *pmap_directory_from_root(uint32_t physical);
bool pmap_root_alloc(pmap_entry_t **directory, uint32_t *root_physical);
void pmap_root_free(uint32_t root_physical);
void pmap_boot_identity(bool enabled);

/* CR0 / CR4 bits the VM layer relies on (start.S sets them). */
#define PMAP_CR0_WP 0x00010000U
#define PMAP_CR0_PG 0x80000000U
#define PMAP_CR4_PSE 0x00000010U

static inline uint32_t
pmap_directory_index(uint32_t virtual_address)
{
	return virtual_address >> 21U;
}

static inline uint32_t
pmap_table_index(uint32_t virtual_address)
{
	return (virtual_address >> 12U) & 0x1FFU;
}

/* Direct-map pointer to a page-table page (or any page) at a physical address. */
static inline pmap_entry_t *
pmap_table_pointer(uint32_t physical_address)
{
	return (pmap_entry_t *)(uintptr_t)(physical_address + VMM_HIGHER_HALF_BASE);
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
pmap_entry_t *pmap_kernel_directory(void);
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
 * directory entry is absent (and not created) or is a 2 MiB page.
 */
bool pmap_get_entry(pmap_entry_t *directory, uint32_t virtual_address, bool create, bool user_tables,
					uint64_t *table_count, pmap_entry_t **entry);

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
