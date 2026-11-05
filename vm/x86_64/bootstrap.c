/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/x86_64/bootstrap.c
 *
 * First x86-64 virtual-memory stage. It replaces the firmware root while
 * retaining an identity map for all objects used by the initial kernel.
 */

#include <vm/x86_64/bootstrap.h>

#include <stdint.h>

#define X86_VM_PAGE_SIZE 4096ULL
#define X86_VM_LARGE_PAGE_SIZE 0x200000ULL
#define X86_VM_BOOTSTRAP_LIMIT 0x40000000ULL
#define X86_VM_TABLE_ENTRIES 512U

#define X86_VM_PRESENT (1ULL << 0)
#define X86_VM_WRITABLE (1ULL << 1)
#define X86_VM_LARGE (1ULL << 7)

#define X86_CR0_PAGING (1UL << 31)
#define X86_CR4_PAE (1UL << 5)
#define X86_CR4_LA57 (1UL << 12)

static uint64_t g_x86_pml4[X86_VM_TABLE_ENTRIES] __attribute__((aligned(X86_VM_PAGE_SIZE)));
static uint64_t g_x86_pdpt[X86_VM_TABLE_ENTRIES] __attribute__((aligned(X86_VM_PAGE_SIZE)));
static uint64_t g_x86_direct_pdpt[X86_VM_TABLE_ENTRIES] __attribute__((aligned(X86_VM_PAGE_SIZE)));
static uint64_t g_x86_page_directory[X86_VM_TABLE_ENTRIES] __attribute__((aligned(X86_VM_PAGE_SIZE)));
static uint64_t g_x86_direct_page_directory[X86_VM_TABLE_ENTRIES] __attribute__((aligned(X86_VM_PAGE_SIZE)));

/*
 * Routine:     x86_vm_read_cr0
 * Purpose:
 *              Read CR0 to verify the processor is already executing with
 *              paging enabled before replacing the firmware's table root.
 */
static uintptr_t
x86_vm_read_cr0(void)
{
	uintptr_t value;

	__asm__ volatile("mov %%cr0, %0" : "=r"(value));

	return value;
}

/*
 * Routine:     x86_vm_read_cr4
 * Purpose:
 *              Check the active paging mode before installing four-level
 *              tables; this bootstrap does not yet support five-level mode.
 */
static uintptr_t
x86_vm_read_cr4(void)
{
	uintptr_t value;

	__asm__ volatile("mov %%cr4, %0" : "=r"(value));

	return value;
}

/*
 * Routine:     x86_vm_identity_address
 * Purpose:
 *              Confirm that a required boot object remains inside the low
 *              identity-mapped window after NXU replaces CR3.
 */
static bool
x86_vm_identity_address(uint64_t address, uint64_t size)
{
	if (address == 0ULL || size == 0ULL || address >= X86_VM_BOOTSTRAP_LIMIT ||
		size > X86_VM_BOOTSTRAP_LIMIT - address) {
		return false;
	}

	return true;
}

/*
 * Routine:     x86_vm_bootstrap
 * Purpose:
 *              Build NXU-owned four-level paging structures, identity-map the
 *              first GiB with 2 MiB pages, and activate them through CR3.
 */
bool
x86_vm_bootstrap(const struct nxu_boot_x86_64 *boot)
{
	uintptr_t cr4 = x86_vm_read_cr4();

	if (boot == 0 || (x86_vm_read_cr0() & X86_CR0_PAGING) == 0UL || (cr4 & X86_CR4_PAE) == 0UL ||
		(cr4 & X86_CR4_LA57) != 0UL) {
		return false;
	}

	if (!x86_vm_identity_address((uint64_t)(uintptr_t)boot, sizeof(*boot)) ||
		!x86_vm_identity_address(boot->memory_map, boot->memory_map_size) ||
		!x86_vm_identity_address(boot->stack_top - 1ULL, 1ULL) ||
		!x86_vm_identity_address((uint64_t)(uintptr_t)g_x86_pml4, X86_VM_PAGE_SIZE) ||
		!x86_vm_identity_address((uint64_t)(uintptr_t)g_x86_pdpt, X86_VM_PAGE_SIZE) ||
		!x86_vm_identity_address((uint64_t)(uintptr_t)g_x86_page_directory, X86_VM_PAGE_SIZE) ||
		boot->memory_descriptor_size < sizeof(uint64_t) ||
		boot->memory_map_size % boot->memory_descriptor_size != 0ULL) {
		return false;
	}

	for (uint32_t index = 0U; index < X86_VM_TABLE_ENTRIES; index++) {
		g_x86_pml4[index] = 0ULL;
		g_x86_pdpt[index] = 0ULL;
		g_x86_direct_pdpt[index] = 0ULL;
		g_x86_page_directory[index] = ((uint64_t)index * X86_VM_LARGE_PAGE_SIZE) |
			X86_VM_PRESENT | X86_VM_WRITABLE | X86_VM_LARGE;
		g_x86_direct_page_directory[index] = g_x86_page_directory[index];
	}

	g_x86_pml4[0] = (uint64_t)(uintptr_t)g_x86_pdpt | X86_VM_PRESENT | X86_VM_WRITABLE;
	g_x86_pdpt[0] = (uint64_t)(uintptr_t)g_x86_page_directory | X86_VM_PRESENT | X86_VM_WRITABLE;
	g_x86_pml4[256] = (uint64_t)(uintptr_t)g_x86_direct_pdpt | X86_VM_PRESENT | X86_VM_WRITABLE;
	g_x86_direct_pdpt[0] =
		(uint64_t)(uintptr_t)g_x86_direct_page_directory | X86_VM_PRESENT | X86_VM_WRITABLE;

	uintptr_t root = (uintptr_t)g_x86_pml4;

	if ((root & (X86_VM_PAGE_SIZE - 1ULL)) != 0ULL) {
		return false;
	}

	__asm__ volatile("mov %0, %%cr3" : : "r"(root) : "memory");

	return true;
}
