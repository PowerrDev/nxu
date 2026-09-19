/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/pmap.c
 *
 * See pmap.h.
 */

#include <mach/i386/pmap.h>

#include <mach/i386/gdt.h>
#include <mach/i386/memory_map.h>

#include <kern/console/console.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

extern uint32_t i386_boot_pd[PMAP_TABLE_ENTRIES];

extern uint8_t __kernel_start[];
extern uint8_t __text_start[];
extern uint8_t __data_start[];
extern uint8_t __kernel_end[];
extern uint8_t i386_boot_stack_guard[];

#define PMAP_MMIO_PAGES (VM_MMIO_SIZE / PMAP_PAGE_SIZE)

/* Directory ranges that get pre-allocated page tables (see pmap.h). */
#define PMAP_ARENA_FIRST_PDE (VM_KERN_BASE >> 22U)
#define PMAP_ARENA_PDES ((uint32_t)(VM_KERN_SIZE >> 22U))
#define PMAP_MMIO_FIRST_PDE (VM_MMIO_BASE >> 22U)
#define PMAP_MMIO_PDES (VM_MMIO_SIZE >> 22U)

typedef struct {
	bool initialized;
	uint32_t directory_physical;
	uint32_t table_count;

	uint8_t mmio_bitmap[PMAP_MMIO_PAGES / 8U];
	uint32_t mmio_pages_in_use;
} pmap_state_t;

static pmap_state_t g_pmap;

static inline void pmap_write_cr3(uint32_t value)
{
	__asm__ volatile("movl %0, %%cr3" : : "r"(value) : "memory");
}

static inline uint32_t pmap_read_cr3(void)
{
	uint32_t value;

	__asm__ volatile("movl %%cr3, %0" : "=r"(value));
	return value;
}

static inline uint32_t pmap_read_cr0(void)
{
	uint32_t value;

	__asm__ volatile("movl %%cr0, %0" : "=r"(value));
	return value;
}

static inline uint32_t pmap_read_cr4(void)
{
	uint32_t value;

	__asm__ volatile("movl %%cr4, %0" : "=r"(value));
	return value;
}

uint32_t pmap_current_directory_physical(void)
{
	return pmap_read_cr3() & PTE_FRAME;
}

void pmap_load_directory(uint32_t directory_physical)
{
	pmap_write_cr3(directory_physical & PTE_FRAME);
}

void pmap_flush_tlb(void)
{
	pmap_write_cr3(pmap_read_cr3());
}

void pmap_invalidate_page(uint32_t virtual_address)
{
	__asm__ volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

static void pmap_rebase(uint32_t *field)
{
	if (*field != 0U && *field < VMM_HIGHER_HALF_BASE) *field += VMM_HIGHER_HALF_BASE;
}

const multiboot_info_t *i386_boot_relocate(uint32_t magic, uint32_t info_physical)
{
	multiboot_info_t *info = 0;

	if (magic == MULTIBOOT_BOOTLOADER_MAGIC && info_physical != 0U && info_physical <= VM_DIRECT_MAP_SIZE - sizeof(multiboot_info_t)) {
		info = (multiboot_info_t *)pmap_table_pointer(info_physical);

		if ((info->flags & MULTIBOOT_INFO_CMDLINE) != 0U) pmap_rebase(&info->cmdline);
		if ((info->flags & MULTIBOOT_INFO_MODS) != 0U && info->mods_count != 0U) pmap_rebase(&info->mods_addr);
		if ((info->flags & MULTIBOOT_INFO_MMAP) != 0U) pmap_rebase(&info->mmap_addr);
	}

	/*
	 * Everything the boot path needs is reachable through the direct map
	 * now; drop the temporary identity mapping so user space starts clean.
	 */
	i386_boot_pd[0] = 0U;
	pmap_flush_tlb();

	return info;
}

uint32_t *pmap_kernel_directory(void)
{
	return i386_boot_pd;
}

uint32_t pmap_kernel_directory_physical(void)
{
	return g_pmap.directory_physical != 0U ? g_pmap.directory_physical : (uint32_t)((uintptr_t)i386_boot_pd - VMM_HIGHER_HALF_BASE);
}

uint32_t pmap_kernel_table_count(void)
{
	return g_pmap.table_count;
}

bool pmap_initialized(void)
{
	return g_pmap.initialized;
}

bool pmap_table_alloc(uint32_t *physical_address)
{
	uint64_t physical;

	if (physical_address == 0 || !pmm_allocate_page(&physical)) return false;

	if (physical >= VM_DIRECT_MAP_SIZE || (physical & PMAP_PAGE_MASK) != 0ULL) {
		(void)pmm_free_page(physical);
		return false;
	}

	*physical_address = (uint32_t)physical;
	return true;
}

void pmap_table_free(uint32_t physical_address)
{
	(void)pmm_free_page(physical_address);
}

bool pmap_get_entry(
	uint32_t *directory,
	uint32_t virtual_address,
	bool create,
	bool user_tables,
	uint64_t *table_count,
	uint32_t **entry
)
{
	if (directory == 0 || entry == 0) return false;

	uint32_t *directory_entry = &directory[pmap_directory_index(virtual_address)];

	if ((*directory_entry & PTE_PRESENT) == 0U) {
		if (!create) return false;

		uint32_t table_physical;

		if (!pmap_table_alloc(&table_physical)) return false;

		*directory_entry = table_physical | PTE_PRESENT | PTE_WRITE | (user_tables ? PTE_USER : 0U);

		if (table_count != 0) (*table_count)++;
	} else if ((*directory_entry & PTE_LARGE) != 0U) {
		return false;
	}

	uint32_t *table = pmap_table_pointer(*directory_entry & PTE_FRAME);

	*entry = &table[pmap_table_index(virtual_address)];
	return true;
}

/* Fill the 4 KiB table for kernel PDE `slot` (a 4 MiB slice of the direct map). */
static void pmap_fill_image_table(uint32_t *table, uint32_t slice)
{
	for (uint32_t page = 0U; page < PMAP_TABLE_ENTRIES; page++) {
		uint32_t physical = slice * PMAP_LARGE_PAGE_SIZE + page * PMAP_PAGE_SIZE;
		uintptr_t virtual_address = (uintptr_t)physical + VMM_HIGHER_HALF_BASE;
		uint32_t flags = PTE_PRESENT | PTE_WRITE;

		/* Boot stub, text and rodata: read-only (CR0.WP enforces it in ring 0). */
		if (virtual_address >= (uintptr_t)__kernel_start && virtual_address < (uintptr_t)__data_start) flags &= ~PTE_WRITE;

		table[page] = physical | flags;

		/* The page below the boot stack stays unmapped (see start.S). */
		if (virtual_address == (uintptr_t)i386_boot_stack_guard) table[page] = 0U;
	}
}

bool pmap_init(void)
{
	if (g_pmap.initialized) return false;

	if ((pmap_read_cr0() & PMAP_CR0_PG) == 0U || (pmap_read_cr0() & PMAP_CR0_WP) == 0U || (pmap_read_cr4() & PMAP_CR4_PSE) == 0U) {
		kputln("pmap_init: paging, write protect or PSE is not enabled");
		return false;
	}

	uint32_t *directory = i386_boot_pd;

	g_pmap.directory_physical = (uint32_t)((uintptr_t)directory - VMM_HIGHER_HALF_BASE);

	if (pmap_current_directory_physical() != g_pmap.directory_physical) {
		kputln("pmap_init: CR3 is not the boot page directory");
		return false;
	}

	/* The boot mapping must be exactly the direct map: user half empty. */
	for (uint32_t index = 0U; index < PMAP_KERNEL_FIRST_PDE; index++) {
		if (directory[index] != 0U) {
			kprintf("pmap_init: stray boot mapping in PDE %u\n", index);
			directory[index] = 0U;
		}
	}

	/* 4 KiB tables under the kernel image, so text and rodata can be read-only. */
	uint32_t image_end = (uint32_t)((uintptr_t)__kernel_end - VMM_HIGHER_HALF_BASE);
	uint32_t image_slices = (image_end + PMAP_LARGE_PAGE_SIZE - 1U) / PMAP_LARGE_PAGE_SIZE;

	if (image_slices > PMAP_DIRECT_MAP_PDES) return false;

	for (uint32_t slice = 0U; slice < image_slices; slice++) {
		uint32_t table_physical;

		if (!pmap_table_alloc(&table_physical)) return false;

		pmap_fill_image_table(pmap_table_pointer(table_physical), slice);

		/* The table maps exactly what the 4 MiB page did, so swapping it in is safe. */
		directory[PMAP_KERNEL_FIRST_PDE + slice] = table_physical | PTE_PRESENT | PTE_WRITE;
		g_pmap.table_count++;
	}

	/* Every other kernel PDE is final too: pre-allocate the arena and MMIO tables. */
	for (uint32_t index = 0U; index < PMAP_ARENA_PDES; index++) {
		uint32_t table_physical;

		if (!pmap_table_alloc(&table_physical)) return false;

		directory[PMAP_ARENA_FIRST_PDE + index] = table_physical | PTE_PRESENT | PTE_WRITE;
		g_pmap.table_count++;
	}

	for (uint32_t index = 0U; index < PMAP_MMIO_PDES; index++) {
		uint32_t table_physical;

		if (!pmap_table_alloc(&table_physical)) return false;

		directory[PMAP_MMIO_FIRST_PDE + index] = table_physical | PTE_PRESENT | PTE_WRITE;
		g_pmap.table_count++;
	}

	pmap_flush_tlb();

	/* The double-fault task switch loads CR3 from its TSS; point it here. */
	i386_gdt_set_double_fault_cr3(g_pmap.directory_physical);

	g_pmap.initialized = true;

	kprintf(
		"pmap_init: kernel directory at phys 0x%x, image %u slice(s), %u table page(s), direct map 0x%x-0x%x\n",
		g_pmap.directory_physical,
		image_slices,
		g_pmap.table_count,
		(uint32_t)VMM_HIGHER_HALF_BASE,
		(uint32_t)(VMM_HIGHER_HALF_BASE + VM_DIRECT_MAP_SIZE)
	);

	return true;
}

static bool pmap_mmio_bit(uint32_t page)
{
	return (g_pmap.mmio_bitmap[page >> 3U] & (1U << (page & 7U))) != 0U;
}

static void pmap_mmio_set_bit(uint32_t page, bool used)
{
	if (used) g_pmap.mmio_bitmap[page >> 3U] |= (uint8_t)(1U << (page & 7U));
	else g_pmap.mmio_bitmap[page >> 3U] &= (uint8_t)~(1U << (page & 7U));
}

static uint32_t pmap_mmio_span(uint32_t offset, uint64_t size)
{
	return (uint32_t)((offset + size + PMAP_PAGE_MASK) / PMAP_PAGE_SIZE);
}

bool pmap_map_mmio(uint64_t physical_address, uint64_t size, void **virtual_address)
{
	if (virtual_address != 0) *virtual_address = 0;

	if (!g_pmap.initialized || virtual_address == 0 || size == 0ULL || size > VM_MMIO_SIZE) return false;
	if (physical_address >= 0x100000000ULL || size > 0x100000000ULL - physical_address) return false;

	uint32_t offset = (uint32_t)(physical_address & PMAP_PAGE_MASK);
	uint32_t pages = pmap_mmio_span(offset, size);
	uint32_t reserved = pages + 1U; /* trailing guard page */

	uint32_t run_start = 0U;
	uint32_t run_length = 0U;
	bool found = false;

	for (uint32_t page = 0U; page < PMAP_MMIO_PAGES; page++) {
		if (pmap_mmio_bit(page)) {
			run_length = 0U;
			continue;
		}

		if (run_length == 0U) run_start = page;
		run_length++;

		if (run_length == reserved) {
			found = true;
			break;
		}
	}

	if (!found) return false;

	uint32_t *directory = pmap_kernel_directory();

	for (uint32_t index = 0U; index < reserved; index++) pmap_mmio_set_bit(run_start + index, true);

	for (uint32_t index = 0U; index < pages; index++) {
		uint32_t page_address = VM_MMIO_BASE + (run_start + index) * PMAP_PAGE_SIZE;
		uint32_t *entry;

		if (!pmap_get_entry(directory, page_address, false, false, 0, &entry)) return false;

		*entry = ((uint32_t)(physical_address & PTE_FRAME) + index * PMAP_PAGE_SIZE) | PTE_PRESENT | PTE_WRITE | PTE_CACHE_DISABLE | PTE_WRITE_THROUGH;
		pmap_invalidate_page(page_address);
	}

	g_pmap.mmio_pages_in_use += reserved;
	*virtual_address = (void *)(uintptr_t)(VM_MMIO_BASE + run_start * PMAP_PAGE_SIZE + offset);
	return true;
}

bool pmap_unmap_mmio(void *virtual_address, uint64_t size)
{
	uintptr_t address = (uintptr_t)virtual_address;

	if (!g_pmap.initialized || !pmap_mmio_contains(virtual_address) || size == 0ULL || size > VM_MMIO_SIZE) return false;

	uint32_t offset = (uint32_t)(address & PMAP_PAGE_MASK);
	uint32_t first = (uint32_t)((address - VM_MMIO_BASE) / PMAP_PAGE_SIZE);
	uint32_t pages = pmap_mmio_span(offset, size);
	uint32_t reserved = pages + 1U;

	if (first + reserved > PMAP_MMIO_PAGES) return false;

	for (uint32_t index = 0U; index < pages; index++) {
		if (!pmap_mmio_bit(first + index)) return false;
	}

	uint32_t *directory = pmap_kernel_directory();

	for (uint32_t index = 0U; index < pages; index++) {
		uint32_t page_address = VM_MMIO_BASE + (first + index) * PMAP_PAGE_SIZE;
		uint32_t *entry;

		if (pmap_get_entry(directory, page_address, false, false, 0, &entry)) {
			*entry = 0U;
			pmap_invalidate_page(page_address);
		}
	}

	for (uint32_t index = 0U; index < reserved; index++) pmap_mmio_set_bit(first + index, false);

	g_pmap.mmio_pages_in_use -= reserved;
	return true;
}

bool pmap_mmio_contains(const void *address)
{
	uintptr_t value = (uintptr_t)address;

	return value >= VM_MMIO_BASE && (uint64_t)value < (uint64_t)VM_MMIO_BASE + VM_MMIO_SIZE;
}

uint32_t pmap_mmio_pages_in_use(void)
{
	return g_pmap.mmio_pages_in_use;
}

void pmap_dump(void)
{
	kprintf(
		"pmap: kernel directory phys 0x%x, %u kernel table page(s), mmio window 0x%x-0x%x, %u page(s) in use\n",
		pmap_kernel_directory_physical(),
		g_pmap.table_count,
		(uint32_t)VM_MMIO_BASE,
		(uint32_t)(VM_MMIO_BASE + (uint32_t)VM_MMIO_SIZE - 1U),
		g_pmap.mmio_pages_in_use
	);
	kprintf(
		"pmap: cr0=0x%x cr3=0x%x cr4=0x%x, image 0x%x-0x%x (text 0x%x, data 0x%x)\n",
		pmap_read_cr0(),
		pmap_read_cr3(),
		pmap_read_cr4(),
		(uint32_t)(uintptr_t)__kernel_start,
		(uint32_t)(uintptr_t)__kernel_end,
		(uint32_t)(uintptr_t)__text_start,
		(uint32_t)(uintptr_t)__data_start
	);
}
