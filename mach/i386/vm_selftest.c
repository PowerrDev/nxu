/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/vm_selftest.c
 *
 * The "vm" phase self-test, run with the boot argument "test=vm". It checks
 * every layer of the VM area against the real hardware, in the order they
 * build on each other:
 *
 *   pmm            page/contiguous allocation, zeroing, refcounts, reservations
 *   direct map     physical <-> virtual aliases, kernel image protections
 *   vm_kern        arena allocation/free, protections, big allocations
 *   kernel maps    vmm_map/protect/unmap/query, uncached mappings, the MMIO window
 *   address spaces two user spaces: independent mappings, CR3 switching, the
 *                  shared kernel half, protections, TLB coherence, teardown
 *   user copy      vm_copy_to_user / vm_copy_from_user across page boundaries
 *   shm / map      the shared kern/tests self-tests, unchanged
 *   heap           kmalloc / kcalloc / kfree, small and large
 *
 * Protections are proven by faulting on purpose: a probe wraps one access in
 * an expected-fault window (see vm_fault.h) and the test then knows the
 * hardware refused it and why (error code). Every access is therefore made
 * from ring 0; user-mode enforcement is exercised once ring 3 exists, but the
 * page-table bits it depends on (U/S, R/W) are the ones checked here.
 *
 * After the checks pass, "vm-fault=<name>" raises one *unexpected* fault so
 * the trap layer's page-fault report can be seen: write-ro, write-text, null,
 * unmapped, user-null, user-ro.
 */

#include <mach/i386/boot_info.h>

#include <mach/i386/memory_map.h>
#include <mach/i386/pmap.h>
#include <mach/i386/vm_fault.h>

#include <kern/console/console.h>
#include <kern/memory/heap.h>
#include <kern/tests/vm_map_test.h>
#include <kern/tests/vm_shm_test.h>
#include <vm/address_space.h>
#include <vm/pmm.h>
#include <vm/user_copy.h>
#include <vm/vm_kern.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern uint8_t __kernel_phys_end[];
extern uint8_t __text_start[];
extern uint8_t __text_end[];
extern uint8_t __rodata_start[];
extern uint8_t __data_start[];
extern uint8_t __kernel_start[];

#define VM_CHECK(condition) \
	do { \
		if (!(condition)) { \
			kprintf("%s: check failed (line %d): %s\n", __func__, __LINE__, #condition); \
			return false; \
		} \
	} while (0)

/* Page-fault error code bits. */
#define VM_FAULT_PRESENT 0x1U
#define VM_FAULT_WRITE 0x2U
#define VM_FAULT_USER 0x4U

#define VM_ARENA_SPARE (VM_KERN_BASE + (uint32_t)VM_KERN_SIZE - 4U * PMAP_PAGE_SIZE)

static uint32_t g_last_fault_error;

/*
 * Store `value` at `address`. True if the store happened; false if it
 * faulted, which the expected-fault window turns into a normal return.
 */
static bool vm_probe_write(uint32_t address, uint32_t value)
{
	volatile i386_vm_fault_expectation_t *expected = &i386_vm_fault_expectation;

	expected->address = address;
	expected->hits = 0U;

	__asm__ volatile(
		"movl $1f, %[fault_eip]\n\t"
		"movl $2f, %[resume_eip]\n\t"
		"movl $1, %[armed]\n\t"
		"1:\tmovl %[value], (%[address])\n\t"
		"2:\n\t"
		"movl $0, %[armed]"
		: [fault_eip] "=m"(expected->fault_eip), [resume_eip] "=m"(expected->resume_eip), [armed] "=m"(expected->armed)
		: [address] "r"(address), [value] "r"(value)
		: "memory"
	);

	g_last_fault_error = expected->error_code;
	return expected->hits == 0U;
}

/* Load a word; false if the load faulted (see vm_probe_write). */
static bool vm_probe_read(uint32_t address, uint32_t *value)
{
	volatile i386_vm_fault_expectation_t *expected = &i386_vm_fault_expectation;
	uint32_t loaded = 0U;

	expected->address = address;
	expected->hits = 0U;

	__asm__ volatile(
		"movl $1f, %[fault_eip]\n\t"
		"movl $2f, %[resume_eip]\n\t"
		"movl $1, %[armed]\n\t"
		"1:\tmovl (%[address]), %[loaded]\n\t"
		"2:\n\t"
		"movl $0, %[armed]"
		: [fault_eip] "=m"(expected->fault_eip), [resume_eip] "=m"(expected->resume_eip), [armed] "=m"(expected->armed), [loaded] "+r"(loaded)
		: [address] "r"(address)
		: "memory"
	);

	g_last_fault_error = expected->error_code;

	if (expected->hits != 0U) return false;

	if (value != 0) *value = loaded;
	return true;
}

static volatile uint32_t *vm_word(uint64_t virtual_address)
{
	return (volatile uint32_t *)(uintptr_t)virtual_address;
}

static bool vm_fill_and_verify(uint64_t virtual_address, uint32_t words, uint32_t seed)
{
	volatile uint32_t *memory = vm_word(virtual_address);

	for (uint32_t index = 0U; index < words; index++) memory[index] = seed + index * 0x9E3779B1U;

	for (uint32_t index = 0U; index < words; index++) {
		if (memory[index] != seed + index * 0x9E3779B1U) return false;
	}

	return true;
}

static bool vm_all_zero(uint64_t virtual_address, uint32_t words)
{
	volatile uint32_t *memory = vm_word(virtual_address);

	for (uint32_t index = 0U; index < words; index++) {
		if (memory[index] != 0U) return false;
	}

	return true;
}

/* --------------------------------------------------------------------------
 * pmm
 * ------------------------------------------------------------------------ */

typedef struct {
	uint64_t base;
	uint64_t size;
} vm_test_range_t;

typedef struct {
	vm_test_range_t ranges[16];
	uint32_t count;
} vm_test_ranges_t;

static void vm_test_collect_range(uint64_t base, uint64_t size, void *context)
{
	vm_test_ranges_t *ranges = (vm_test_ranges_t *)context;

	if (ranges->count < 16U) {
		ranges->ranges[ranges->count].base = base;
		ranges->ranges[ranges->count].size = size;
		ranges->count++;
	}
}

static bool vm_test_pmm(const i386_boot_info_t *boot)
{
	uint64_t free_before = pmm_get_free_page_count();
	uint64_t used_before = pmm_get_used_page_count();
	uint64_t page;
	uint64_t virtual_address;

	VM_CHECK(free_before + used_before == pmm_get_page_count());

	/* Everything the boot path put in RAM must be reserved: image, loader data. */
	for (uint64_t address = 0x100000ULL; address < (uintptr_t)__kernel_phys_end; address += PMM_PAGE_SIZE) {
		VM_CHECK(pmm_page_refcount(address) != 0U);
	}

	vm_test_ranges_t boot_ranges = { .count = 0U };

	VM_CHECK(i386_memory_map_boot_ranges(boot->multiboot, vm_test_collect_range, &boot_ranges) >= 2U);

	for (uint32_t index = 0U; index < boot_ranges.count; index++) {
		uint64_t start = boot_ranges.ranges[index].base & ~(PMM_PAGE_SIZE - 1ULL);
		uint64_t end = boot_ranges.ranges[index].base + boot_ranges.ranges[index].size;

		for (uint64_t address = start; address < end; address += PMM_PAGE_SIZE) {
			if (address >= 0x100000ULL && address < VM_DIRECT_MAP_SIZE) VM_CHECK(pmm_page_refcount(address) != 0U);
		}
	}

	/* The command line the boot path handed out is still intact. */
	char value[16];

	VM_CHECK(i386_boot_arg("test", value, sizeof(value)));
	VM_CHECK(strcmp(value, "vm") == 0);

	/* One page: aligned, in RAM, zeroed, refcounted. */
	VM_CHECK(pmm_allocate_page(&page));
	VM_CHECK((page & (PMM_PAGE_SIZE - 1ULL)) == 0ULL);
	VM_CHECK(page >= 0x100000ULL && page < VM_DIRECT_MAP_SIZE);
	VM_CHECK(page >= (uintptr_t)__kernel_phys_end);
	VM_CHECK(pmm_page_refcount(page) == 1U);
	VM_CHECK(pmm_get_free_page_count() == free_before - 1ULL);
	VM_CHECK(pmm_get_used_page_count() == used_before + 1ULL);

	VM_CHECK(vmm_physical_to_higher_half(page, &virtual_address));
	VM_CHECK(vm_all_zero(virtual_address, 1024U));
	VM_CHECK(vm_fill_and_verify(virtual_address, 1024U, 0x1234U));

	VM_CHECK(pmm_free_page(page));
	VM_CHECK(!pmm_free_page(page));
	VM_CHECK(pmm_page_refcount(page) == 0U);
	VM_CHECK(pmm_get_free_page_count() == free_before);

	/* A recycled page comes back zeroed even though it was dirty when freed. */
	uint64_t again;

	VM_CHECK(pmm_allocate_page(&again));
	VM_CHECK(vmm_physical_to_higher_half(again, &virtual_address));
	VM_CHECK(vm_all_zero(virtual_address, 1024U));
	VM_CHECK(pmm_free_page(again));

	/* Misuse is refused. */
	VM_CHECK(!pmm_free_page(0x100001ULL));
	VM_CHECK(!pmm_free_page(VM_DIRECT_MAP_SIZE + PMM_PAGE_SIZE));
	VM_CHECK(!pmm_page_retain(page));

	/* Many pages: all distinct, none inside the reserved image. */
	uint64_t pages[96];

	for (uint32_t index = 0U; index < 96U; index++) {
		VM_CHECK(pmm_allocate_page(&pages[index]));
		VM_CHECK(pages[index] >= (uintptr_t)__kernel_phys_end);

		for (uint32_t other = 0U; other < index; other++) VM_CHECK(pages[other] != pages[index]);
	}

	for (uint32_t index = 0U; index < 96U; index += 2U) VM_CHECK(pmm_free_page(pages[index]));
	for (uint32_t index = 1U; index < 96U; index += 2U) VM_CHECK(pmm_free_page(pages[index]));

	VM_CHECK(pmm_get_free_page_count() == free_before);

	/* Contiguous runs are physically contiguous and contiguous through the direct map. */
	uint64_t run;

	VM_CHECK(pmm_allocate_contiguous_pages(8ULL, &run));
	VM_CHECK((run & (PMM_PAGE_SIZE - 1ULL)) == 0ULL);
	VM_CHECK(pmm_get_free_page_count() == free_before - 8ULL);

	for (uint32_t index = 0U; index < 8U; index++) VM_CHECK(pmm_page_refcount(run + index * PMM_PAGE_SIZE) == 1U);

	VM_CHECK(vmm_physical_to_higher_half(run, &virtual_address));
	VM_CHECK(vm_all_zero(virtual_address, 8U * 1024U));
	VM_CHECK(vm_fill_and_verify(virtual_address, 8U * 1024U, 0xC0DEU));
	VM_CHECK(!pmm_free_contiguous_pages(run + 1ULL, 8ULL));
	VM_CHECK(!pmm_free_contiguous_pages(run, 0ULL));
	VM_CHECK(pmm_free_contiguous_pages(run, 8ULL));
	VM_CHECK(pmm_get_free_page_count() == free_before);
	VM_CHECK(!pmm_allocate_contiguous_pages(0ULL, &run));
	VM_CHECK(!pmm_allocate_contiguous_pages(pmm_get_page_count() + 1ULL, &run));

	/* Extra owners: the page survives until the last one lets go. */
	VM_CHECK(pmm_allocate_page(&page));
	VM_CHECK(pmm_page_retain(page));
	VM_CHECK(pmm_page_refcount(page) == 2U);
	VM_CHECK(pmm_free_page(page));
	VM_CHECK(pmm_page_refcount(page) == 1U);
	VM_CHECK(pmm_free_page(page));
	VM_CHECK(pmm_page_refcount(page) == 0U);
	VM_CHECK(pmm_get_free_page_count() == free_before);
	VM_CHECK(pmm_get_used_page_count() == used_before);

	kprintf("vm_test_pmm: passed (%llu pages, %llu free)\n", pmm_get_page_count(), pmm_get_free_page_count());
	return true;
}

/* --------------------------------------------------------------------------
 * direct map and the kernel image
 * ------------------------------------------------------------------------ */

static bool vm_test_direct_map(const platform_t *platform)
{
	uint64_t page;
	uint64_t alias;
	uint64_t back;
	uint64_t translated;

	VM_CHECK(vmm_is_enabled());
	VM_CHECK(vmm_higher_half_enabled());
	VM_CHECK(vmm_higher_half_direct_map_enabled());
	VM_CHECK(vmm_validate_linked_kernel_layout());
	VM_CHECK(vmm_validate_kernel_permissions());
	VM_CHECK(vmm_validate_higher_half_direct_map(platform));

	/* Address classification and conversions at the edges. */
	VM_CHECK(vmm_is_higher_half_address(0xC0000000ULL));
	VM_CHECK(vmm_is_higher_half_address(0xFFFFF000ULL));
	VM_CHECK(!vmm_is_higher_half_address(0xBFFFFFFFULL));
	VM_CHECK(!vmm_is_higher_half_address(0ULL));

	VM_CHECK(vmm_physical_to_higher_half(0ULL, &alias) && alias == 0xC0000000ULL);
	VM_CHECK(vmm_physical_to_higher_half(VM_DIRECT_MAP_SIZE - 1ULL, &alias) && alias == 0xEFFFFFFFULL);
	VM_CHECK(!vmm_physical_to_higher_half(VM_DIRECT_MAP_SIZE, &alias));
	VM_CHECK(!vmm_higher_half_to_physical(0xBFFFFFFFULL, &back));
	VM_CHECK(!vmm_higher_half_to_physical(0xF0000000ULL, &back));
	VM_CHECK(vmm_higher_half_to_physical(0xC0100000ULL, &back) && back == 0x100000ULL);

	/* The image's own symbols resolve to where the loader put it. */
	VM_CHECK(vmm_kernel_address_to_physical((uintptr_t)__kernel_start, &back) && back == 0x100000ULL);
	VM_CHECK(vmm_kernel_address_to_physical((uintptr_t)&translated, &back));
	VM_CHECK(vmm_kernel_address_to_physical(0x200000ULL, &back) && back == 0x200000ULL);
	VM_CHECK(!vmm_kernel_address_to_physical(0xBFFFF000ULL, &back));

	/* Kernel image protections: text and rodata read-only, data writable. */
	VM_CHECK(vmm_translate((uintptr_t)__text_start, &translated));
	VM_CHECK(!vmm_translate_write((uintptr_t)__text_start, &translated));
	VM_CHECK(!vmm_translate_write((uintptr_t)__rodata_start, &translated));
	VM_CHECK(vmm_translate_write((uintptr_t)&translated & ~(uint64_t)PMAP_PAGE_MASK, &translated));

	vmm_page_mapping_t mapping;

	VM_CHECK(vmm_query_page((uintptr_t)__text_start, &mapping));
	VM_CHECK(mapping.protection != VMM_PROTECTION_READ_WRITE && mapping.physical_address == 0x101000ULL);

	/* Hardware really refuses a write to text (CR0.WP), and the page is untouched. */
	uint32_t before;

	VM_CHECK(vm_probe_read((uintptr_t)__text_start, &before));
	VM_CHECK(!vm_probe_write((uintptr_t)__text_start, before ^ 0xFFFFFFFFU));
	VM_CHECK((g_last_fault_error & (VM_FAULT_PRESENT | VM_FAULT_WRITE)) == (VM_FAULT_PRESENT | VM_FAULT_WRITE));
	VM_CHECK((g_last_fault_error & VM_FAULT_USER) == 0U);

	uint32_t after;

	VM_CHECK(vm_probe_read((uintptr_t)__text_start, &after) && after == before);

	/* A fresh page seen through the direct map: write via one alias, read via the others. */
	VM_CHECK(pmm_allocate_page(&page));
	VM_CHECK(vmm_physical_to_higher_half(page, &alias));
	VM_CHECK(vmm_higher_half_to_physical(alias, &back) && back == page);
	VM_CHECK(vmm_kernel_address_to_physical(alias, &back) && back == page);
	VM_CHECK(vmm_translate(alias + 0x123ULL, &translated) && translated == page + 0x123ULL);
	VM_CHECK(vmm_translate_write(alias, &translated) && translated == page);

	vmm_page_mapping_t direct_mapping;

	VM_CHECK(vmm_query_page(alias, &direct_mapping));
	VM_CHECK(direct_mapping.physical_address == page && direct_mapping.protection == VMM_PROTECTION_READ_WRITE);
	VM_CHECK(direct_mapping.memory_type == VMM_MEMORY_NORMAL);

	VM_CHECK(vm_fill_and_verify(alias, 1024U, 0xD1EC7U));

	/* A second, independent alias of the same physical page (arena mapping). */
	VM_CHECK(vmm_map_page(VM_ARENA_SPARE, page, VMM_MEMORY_NORMAL, VMM_PROTECTION_READ_WRITE));

	volatile uint32_t *second = vm_word(VM_ARENA_SPARE);
	volatile uint32_t *first = vm_word(alias);

	for (uint32_t index = 0U; index < 1024U; index++) VM_CHECK(second[index] == 0xD1EC7U + index * 0x9E3779B1U);

	second[7] = 0xA11A5U;
	VM_CHECK(first[7] == 0xA11A5U);
	first[9] = 0x5EEDU;
	VM_CHECK(second[9] == 0x5EEDU);

	uint64_t unmapped;

	VM_CHECK(vmm_unmap_page(VM_ARENA_SPARE, &unmapped) && unmapped == page);
	VM_CHECK(pmm_free_page(page));

	/* The top of RAM is reachable through the direct map. */
	const platform_region_t *last = &platform->memory_regions[platform->memory_region_count - 1U];
	uint32_t top_word;

	VM_CHECK(vmm_physical_to_higher_half(last->base + last->size - 4ULL, &alias));
	VM_CHECK(vm_probe_read((uint32_t)alias, &top_word));

	/* The boot stack is usable to its last byte, and the page below it faults. */
	extern uint8_t i386_boot_stack_guard[];
	extern uint8_t i386_boot_stack_bottom[];

	VM_CHECK((uintptr_t)i386_boot_stack_guard + PMAP_PAGE_SIZE == (uintptr_t)i386_boot_stack_bottom);
	VM_CHECK(vm_probe_read((uint32_t)(uintptr_t)i386_boot_stack_bottom, &top_word));
	VM_CHECK(!vm_probe_read((uint32_t)(uintptr_t)i386_boot_stack_guard + PMAP_PAGE_SIZE - 4U, 0));
	VM_CHECK(!vm_probe_write((uint32_t)(uintptr_t)i386_boot_stack_guard, 1U));
	VM_CHECK((g_last_fault_error & VM_FAULT_PRESENT) == 0U);
	VM_CHECK(!vmm_translate((uintptr_t)i386_boot_stack_guard, &translated));

	/* Nothing lives in the user half or the reserved range of the master directory. */
	const uint32_t *directory = pmap_kernel_directory();

	for (uint32_t index = 0U; index < PMAP_KERNEL_FIRST_PDE; index++) VM_CHECK(directory[index] == 0U);
	for (uint32_t index = 976U; index < 992U; index++) VM_CHECK(directory[index] == 0U);
	VM_CHECK(!vm_probe_read(VM_KERN_BASE + (uint32_t)VM_KERN_SIZE + PMAP_PAGE_SIZE, 0));

	kprintf("vm_test_direct_map: passed (text read-only, data writable, aliases coherent)\n");
	return true;
}

/* --------------------------------------------------------------------------
 * vm_kern
 * ------------------------------------------------------------------------ */

static bool vm_test_vm_kern(void)
{
	uint64_t free_before = pmm_get_free_page_count();
	uint64_t arena_used_before = vm_kern_get_used_pages();
	uint64_t allocations_before = vm_kern_get_allocation_count();
	void *one;
	void *many;
	void *big;

	VM_CHECK(vm_kern_get_total_pages() == VM_KERN_SIZE / PMM_PAGE_SIZE);

	/* One page: in the arena, zeroed, writable, physically backed. */
	VM_CHECK(vm_kern_allocate(1U, VMM_PROTECTION_READ_WRITE, &one));
	VM_CHECK(vm_kern_contains(one));
	VM_CHECK(((uintptr_t)one & PMAP_PAGE_MASK) == 0U);
	VM_CHECK(vm_all_zero((uintptr_t)one, 1024U));
	VM_CHECK(vm_fill_and_verify((uintptr_t)one, 1024U, 0x0111U));
	VM_CHECK(vm_kern_get_used_pages() == arena_used_before + 1ULL);
	VM_CHECK(pmm_get_free_page_count() == free_before - 1ULL);

	vmm_page_mapping_t mapping;

	VM_CHECK(vmm_query_page((uintptr_t)one, &mapping));
	VM_CHECK(mapping.protection == VMM_PROTECTION_READ_WRITE && mapping.memory_type == VMM_MEMORY_NORMAL);
	VM_CHECK(pmm_page_refcount(mapping.physical_address) == 1U);

	/* The arena page and the direct map see the same bytes. */
	uint64_t alias;

	VM_CHECK(vmm_physical_to_higher_half(mapping.physical_address, &alias));
	VM_CHECK(vm_word(alias)[5] == 0x0111U + 5U * 0x9E3779B1U);

	/* Sixteen pages: virtually contiguous, each page independent. */
	VM_CHECK(vm_kern_allocate(16U * PMM_PAGE_SIZE, VMM_PROTECTION_READ_WRITE, &many));
	VM_CHECK(vm_kern_get_allocation_count() == allocations_before + 2ULL);
	VM_CHECK(vm_fill_and_verify((uintptr_t)many, 16U * 1024U, 0x0222U));

	/* Rounded up to whole pages; a wrong size does not free it. */
	void *odd;

	VM_CHECK(vm_kern_allocate(PMM_PAGE_SIZE + 1U, VMM_PROTECTION_READ_WRITE, &odd));
	VM_CHECK(vm_kern_get_used_pages() == arena_used_before + 1ULL + 16ULL + 2ULL);
	VM_CHECK(!vm_kern_free(odd, PMM_PAGE_SIZE));
	VM_CHECK(!vm_kern_free((uint8_t *)odd + 1, PMM_PAGE_SIZE + 1U));
	VM_CHECK(vm_kern_free(odd, PMM_PAGE_SIZE + 1U));

	/* 5 MiB: crosses page-table (4 MiB) boundaries inside the arena. */
	VM_CHECK(vm_kern_allocate(5U * 1024U * 1024U, VMM_PROTECTION_READ_WRITE, &big));

	for (uint32_t page = 0U; page < 1280U; page += 7U) {
		volatile uint32_t *word = vm_word((uintptr_t)big + page * PMM_PAGE_SIZE);

		*word = 0xB16B00B5U ^ page;
	}

	for (uint32_t page = 0U; page < 1280U; page += 7U) VM_CHECK(*vm_word((uintptr_t)big + page * PMM_PAGE_SIZE) == (0xB16B00B5U ^ page));

	VM_CHECK(vm_kern_free(big, 5U * 1024U * 1024U));

	/* A freed page is gone from the page tables and faults. */
	VM_CHECK(!vm_probe_read((uint32_t)(uintptr_t)big, 0));
	VM_CHECK((g_last_fault_error & VM_FAULT_PRESENT) == 0U);

	/* Read-only arena memory: readable, refuses stores. */
	void *readonly;

	VM_CHECK(vm_kern_allocate(PMM_PAGE_SIZE, VMM_PROTECTION_READ_ONLY, &readonly));
	VM_CHECK(vmm_query_page((uintptr_t)readonly, &mapping) && mapping.protection == VMM_PROTECTION_READ_ONLY);
	VM_CHECK(vm_all_zero((uintptr_t)readonly, 1024U));
	VM_CHECK(!vm_probe_write((uint32_t)(uintptr_t)readonly, 1U));
	VM_CHECK((g_last_fault_error & (VM_FAULT_PRESENT | VM_FAULT_WRITE)) == (VM_FAULT_PRESENT | VM_FAULT_WRITE));
	VM_CHECK(vm_all_zero((uintptr_t)readonly, 1024U));
	VM_CHECK(vm_kern_free(readonly, PMM_PAGE_SIZE));

	/* Read-execute is read-only in hardware but remembered as such. */
	void *executable;

	VM_CHECK(vm_kern_allocate(PMM_PAGE_SIZE, VMM_PROTECTION_READ_EXECUTE, &executable));
	VM_CHECK(vmm_query_page((uintptr_t)executable, &mapping) && mapping.protection == VMM_PROTECTION_READ_EXECUTE);
	VM_CHECK(!vm_probe_write((uint32_t)(uintptr_t)executable, 1U));
	VM_CHECK(vm_kern_free(executable, PMM_PAGE_SIZE));

	/* The arena's own bookkeeping refuses nonsense. */
	void *none;

	VM_CHECK(!vm_kern_allocate(0U, VMM_PROTECTION_READ_WRITE, &none));
	VM_CHECK(!vm_kern_allocate((size_t)VM_KERN_SIZE + 1U, VMM_PROTECTION_READ_WRITE, &none));
	VM_CHECK(!vm_kern_free(0, PMM_PAGE_SIZE));
	VM_CHECK(!vm_kern_free((void *)(uintptr_t)VM_KERN_BASE, PMM_PAGE_SIZE * 4096U));
	VM_CHECK(!vm_kern_contains((void *)0xC0100000U));
	VM_CHECK(!vm_kern_contains((void *)(uintptr_t)(VM_KERN_END)));

	VM_CHECK(vm_kern_free(many, 16U * PMM_PAGE_SIZE));
	VM_CHECK(!vm_kern_free(many, 16U * PMM_PAGE_SIZE));
	VM_CHECK(vm_kern_free(one, 1U));

	VM_CHECK(vm_kern_get_used_pages() == arena_used_before);
	VM_CHECK(vm_kern_get_allocation_count() == allocations_before);
	VM_CHECK(pmm_get_free_page_count() == free_before);

	kprintf("vm_test_vm_kern: passed (single, multi-page, 5 MiB, read-only, exact accounting)\n");
	return true;
}

/* --------------------------------------------------------------------------
 * kernel mappings and the MMIO window
 * ------------------------------------------------------------------------ */

static bool vm_test_kernel_mappings(void)
{
	uint64_t page;
	uint64_t unmapped;
	vmm_page_mapping_t mapping;
	uint32_t value;

	VM_CHECK(pmm_allocate_page(&page));
	VM_CHECK(vm_fill_and_verify(page + VMM_HIGHER_HALF_BASE, 1024U, 0x77U));

	/* Nothing is mapped at the spare arena address yet. */
	VM_CHECK(!vmm_query_page(VM_ARENA_SPARE, &mapping));
	VM_CHECK(!vmm_unmap_page(VM_ARENA_SPARE, &unmapped));
	VM_CHECK(!vm_probe_read((uint32_t)VM_ARENA_SPARE, 0));
	VM_CHECK((g_last_fault_error & VM_FAULT_PRESENT) == 0U);

	/* Addresses the kernel may not remap: misaligned, user half, direct map, image. */
	VM_CHECK(!vmm_map_page(VM_ARENA_SPARE + 4ULL, page, VMM_MEMORY_NORMAL, VMM_PROTECTION_READ_WRITE));
	VM_CHECK(!vmm_map_page(VM_ARENA_SPARE, page + 4ULL, VMM_MEMORY_NORMAL, VMM_PROTECTION_READ_WRITE));
	VM_CHECK(!vmm_map_page(0x1000ULL, page, VMM_MEMORY_NORMAL, VMM_PROTECTION_READ_WRITE));
	VM_CHECK(!vmm_map_page(0ULL, page, VMM_MEMORY_NORMAL, VMM_PROTECTION_READ_WRITE));
	VM_CHECK(!vmm_map_page(0xC0800000ULL, page, VMM_MEMORY_NORMAL, VMM_PROTECTION_READ_WRITE));
	VM_CHECK(!vmm_map_page(0xC0101000ULL, page, VMM_MEMORY_NORMAL, VMM_PROTECTION_READ_WRITE));
	VM_CHECK(!vmm_map_page(VM_ARENA_SPARE, 0x100000000ULL, VMM_MEMORY_NORMAL, VMM_PROTECTION_READ_WRITE));
	VM_CHECK(!vmm_map_page(0xF4000000ULL, page, VMM_MEMORY_NORMAL, VMM_PROTECTION_READ_WRITE));
	VM_CHECK(!vmm_unmap_page(0xC0101000ULL, &unmapped));

	/* Map read-write: contents visible, query agrees, double map refused. */
	VM_CHECK(vmm_map_page(VM_ARENA_SPARE, page, VMM_MEMORY_NORMAL, VMM_PROTECTION_READ_WRITE));
	VM_CHECK(!vmm_map_page(VM_ARENA_SPARE, page, VMM_MEMORY_NORMAL, VMM_PROTECTION_READ_WRITE));
	VM_CHECK(vmm_query_page(VM_ARENA_SPARE, &mapping));
	VM_CHECK(mapping.physical_address == page && mapping.protection == VMM_PROTECTION_READ_WRITE && mapping.memory_type == VMM_MEMORY_NORMAL);
	VM_CHECK(vm_probe_read((uint32_t)VM_ARENA_SPARE + 12U, &value) && value == 0x77U + 3U * 0x9E3779B1U);
	VM_CHECK(vm_probe_write((uint32_t)VM_ARENA_SPARE, 0xFEEDF00DU));

	uint64_t translated;

	VM_CHECK(vmm_translate(VM_ARENA_SPARE + 0x44ULL, &translated) && translated == page + 0x44ULL);
	VM_CHECK(vmm_translate_write(VM_ARENA_SPARE, &translated) && translated == page);
	VM_CHECK(vmm_kernel_address_to_physical(VM_ARENA_SPARE + 8ULL, &translated) && translated == page + 8ULL);

	/* Protect to read-only: reads work (and see the store), writes fault, nothing changes. */
	VM_CHECK(vmm_protect_page(VM_ARENA_SPARE, VMM_PROTECTION_READ_ONLY));
	VM_CHECK(vmm_query_page(VM_ARENA_SPARE, &mapping) && mapping.protection == VMM_PROTECTION_READ_ONLY);
	VM_CHECK(vmm_translate(VM_ARENA_SPARE, &translated));
	VM_CHECK(!vmm_translate_write(VM_ARENA_SPARE, &translated));
	VM_CHECK(vm_probe_read((uint32_t)VM_ARENA_SPARE, &value) && value == 0xFEEDF00DU);
	VM_CHECK(!vm_probe_write((uint32_t)VM_ARENA_SPARE, 0xBAD0BAD0U));
	VM_CHECK((g_last_fault_error & (VM_FAULT_PRESENT | VM_FAULT_WRITE)) == (VM_FAULT_PRESENT | VM_FAULT_WRITE));
	VM_CHECK(vm_probe_read((uint32_t)VM_ARENA_SPARE, &value) && value == 0xFEEDF00DU);

	/* Read-execute: also read-only in hardware, distinguishable in queries. */
	VM_CHECK(vmm_protect_page(VM_ARENA_SPARE, VMM_PROTECTION_READ_EXECUTE));
	VM_CHECK(vmm_query_page(VM_ARENA_SPARE, &mapping) && mapping.protection == VMM_PROTECTION_READ_EXECUTE);
	VM_CHECK(!vm_probe_write((uint32_t)VM_ARENA_SPARE, 1U));

	/* Back to read-write: the store now works (the stale read-only TLB entry is gone). */
	VM_CHECK(vmm_protect_page(VM_ARENA_SPARE, VMM_PROTECTION_READ_WRITE));
	VM_CHECK(vm_probe_write((uint32_t)VM_ARENA_SPARE, 0x600DU));
	VM_CHECK(vm_word(page + VMM_HIGHER_HALF_BASE)[0] == 0x600DU);
	VM_CHECK(!vmm_protect_page(VM_ARENA_SPARE + 4ULL, VMM_PROTECTION_READ_ONLY));
	VM_CHECK(!vmm_protect_page(VM_ARENA_SPARE + 0x1000ULL, VMM_PROTECTION_READ_ONLY));

	/* Unmap: the physical page comes back, the address faults, the page lives on. */
	VM_CHECK(vmm_unmap_page(VM_ARENA_SPARE, &unmapped) && unmapped == page);
	VM_CHECK(!vmm_unmap_page(VM_ARENA_SPARE, &unmapped));
	VM_CHECK(!vmm_query_page(VM_ARENA_SPARE, &mapping));
	VM_CHECK(!vm_probe_read((uint32_t)VM_ARENA_SPARE, 0));
	VM_CHECK((g_last_fault_error & VM_FAULT_PRESENT) == 0U);
	VM_CHECK(!vm_probe_write((uint32_t)VM_ARENA_SPARE, 1U));
	VM_CHECK(!vmm_translate(VM_ARENA_SPARE, &translated));
	VM_CHECK(pmm_page_refcount(page) == 1U);

	/* An uncached (device) mapping reports its type. */
	VM_CHECK(vmm_map_page(VM_ARENA_SPARE, page, VMM_MEMORY_DEVICE, VMM_PROTECTION_READ_WRITE));
	VM_CHECK(vmm_query_page(VM_ARENA_SPARE, &mapping) && mapping.memory_type == VMM_MEMORY_DEVICE);
	VM_CHECK(vm_probe_read((uint32_t)VM_ARENA_SPARE, &value) && value == 0x600DU);
	VM_CHECK(vmm_unmap_page(VM_ARENA_SPARE, 0));
	VM_CHECK(pmm_free_page(page));

	/* The MMIO window: uncached alias of RAM (offset preserved), then a real device range. */
	VM_CHECK(pmm_allocate_page(&page));
	VM_CHECK(vm_fill_and_verify(page + VMM_HIGHER_HALF_BASE, 1024U, 0x33U));

	void *mmio;

	VM_CHECK(pmap_mmio_pages_in_use() == 0U);
	VM_CHECK(pmap_map_mmio(page + 0x10ULL, 0x40ULL, &mmio));
	VM_CHECK(pmap_mmio_contains(mmio));
	VM_CHECK(((uintptr_t)mmio & PMAP_PAGE_MASK) == 0x10U);
	VM_CHECK(pmap_mmio_pages_in_use() == 2U);
	VM_CHECK(vmm_query_page((uintptr_t)mmio, &mapping));
	VM_CHECK(mapping.memory_type == VMM_MEMORY_DEVICE && mapping.physical_address == page);
	VM_CHECK(*(volatile uint32_t *)mmio == 0x33U + 4U * 0x9E3779B1U);
	*(volatile uint32_t *)mmio = 0x0DD5U;
	VM_CHECK(vm_word(page + VMM_HIGHER_HALF_BASE)[4] == 0x0DD5U);

	/* The page after a mapping is an unmapped guard. */
	VM_CHECK(!vm_probe_read((uint32_t)(((uintptr_t)mmio & ~(uintptr_t)PMAP_PAGE_MASK) + PMAP_PAGE_SIZE), 0));

	/* Above the direct map: the local APIC page, which QEMU's PC machine has. */
	void *apic;

	VM_CHECK(pmap_map_mmio(0xFEE00000ULL, PMAP_PAGE_SIZE, &apic));
	VM_CHECK(vm_probe_read((uint32_t)(uintptr_t)apic + 0x20U, &value));
	VM_CHECK(pmap_unmap_mmio(apic, PMAP_PAGE_SIZE));

	VM_CHECK(!pmap_map_mmio(0x100000000ULL, 4U, &apic));
	VM_CHECK(!pmap_map_mmio(0xFFFFFFF0ULL, 0x100ULL, &apic));
	VM_CHECK(!pmap_map_mmio(page, 0ULL, &apic));
	VM_CHECK(!pmap_unmap_mmio((void *)(uintptr_t)(VM_MMIO_BASE + 0x100000U), 0x1000ULL));

	VM_CHECK(pmap_unmap_mmio(mmio, 0x40ULL));
	VM_CHECK(!pmap_unmap_mmio(mmio, 0x40ULL));
	VM_CHECK(pmap_mmio_pages_in_use() == 0U);
	VM_CHECK(!vm_probe_read((uint32_t)(uintptr_t)mmio, 0));
	VM_CHECK(pmm_free_page(page));

	kprintf("vm_test_kernel_mappings: passed (map, protect, unmap, device type, mmio window)\n");
	return true;
}

/* --------------------------------------------------------------------------
 * user address spaces
 * ------------------------------------------------------------------------ */

#define VM_USER_A 0x00400000U
#define VM_USER_RO 0x00401000U
#define VM_USER_RX 0x00402000U
#define VM_USER_FAR 0x08000000U
#define VM_USER_COPY 0x00800000U

static bool vm_test_address_spaces(void)
{
	uint64_t free_before = pmm_get_free_page_count();
	vm_address_space_t space_a = { 0 };
	vm_address_space_t space_b = { 0 };
	uint64_t page_a;
	uint64_t page_b;
	uint64_t page_c;
	uint32_t value;

	VM_CHECK(vm_address_space_create(&space_a));
	VM_CHECK(vm_address_space_create(&space_b));
	VM_CHECK(!vm_address_space_create(&space_a));
	VM_CHECK(vm_address_space_table_count(&space_a) == 1ULL);
	VM_CHECK((vm_address_space_root_physical(&space_a) & PMAP_PAGE_MASK) == 0ULL);
	VM_CHECK(vm_address_space_root_physical(&space_a) != vm_address_space_root_physical(&space_b));
	VM_CHECK(vm_address_space_root_physical(&space_a) != pmap_kernel_directory_physical());
	VM_CHECK(vm_address_space_current() == 0 && !vm_address_space_is_active(&space_a));
	VM_CHECK(vm_address_space_deactivate());
	VM_CHECK(pmap_current_directory_physical() == pmap_kernel_directory_physical());

	/* Kernel half identical to the master, user half empty. */
	const uint32_t *master = pmap_kernel_directory();
	const uint32_t *directory_a = (const uint32_t *)space_a.root;
	const uint32_t *directory_b = (const uint32_t *)space_b.root;

	for (uint32_t index = 0U; index < PMAP_KERNEL_FIRST_PDE; index++) VM_CHECK(directory_a[index] == 0U && directory_b[index] == 0U);
	for (uint32_t index = PMAP_KERNEL_FIRST_PDE; index < PMAP_TABLE_ENTRIES; index++) VM_CHECK(directory_a[index] == master[index] && directory_b[index] == master[index]);

	VM_CHECK(pmm_allocate_page(&page_a));
	VM_CHECK(pmm_allocate_page(&page_b));
	VM_CHECK(pmm_allocate_page(&page_c));

	/* Limits: null guard, alignment, the kernel half, physical pages outside RAM. */
	VM_CHECK(!vm_address_space_map_page(&space_a, 0ULL, page_a, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(!vm_address_space_map_page(&space_a, 0x800ULL, page_a, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(!vm_address_space_map_page(&space_a, VM_USER_A + 4ULL, page_a, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(!vm_address_space_map_page(&space_a, VM_USER_A, page_a + 4ULL, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(!vm_address_space_map_page(&space_a, VM_MAX_USER_ADDRESS, page_a, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(!vm_address_space_map_page(&space_a, 0xC0000000ULL, page_a, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(!vm_address_space_map_page(&space_a, VM_KERN_BASE, page_a, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(!vm_address_space_map_page(&space_a, 0xFFFFF000ULL, page_a, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(!vm_address_space_map_page(&space_a, VM_USER_A, VM_DIRECT_MAP_SIZE, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(!vm_address_space_map_page(&space_a, 0x100000000ULL + VM_USER_A, page_a, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(!vm_address_space_unmap_page(&space_a, 0ULL));
	VM_CHECK(!vm_address_space_unmap_page(&space_a, VM_USER_A));
	VM_CHECK(vm_address_space_table_count(&space_a) == 1ULL);

	/* The highest user page is mappable; the first page below the limit is the last. */
	VM_CHECK(vm_address_space_map_page(&space_a, VM_USER_STACK_TOP, page_c, VM_USER_PROTECTION_READ_WRITE));

	vm_user_page_mapping_t mapping;

	VM_CHECK(vm_address_space_query_page(&space_a, VM_USER_STACK_TOP + 0x123ULL, &mapping) && mapping.physical_address == page_c);
	VM_CHECK(vm_address_space_table_count(&space_a) == 2ULL);
	VM_CHECK(vm_address_space_unmap_page(&space_a, VM_USER_STACK_TOP));
	VM_CHECK(!vm_address_space_query_page(&space_a, VM_USER_STACK_TOP, &mapping));

	/* Same user address, different pages, in two spaces. */
	VM_CHECK(vm_address_space_map_page(&space_a, VM_USER_A, page_a, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(vm_address_space_map_page(&space_b, VM_USER_A, page_b, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(!vm_address_space_map_page(&space_a, VM_USER_A, page_b, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(vm_address_space_table_count(&space_a) == 3ULL && vm_address_space_table_count(&space_b) == 2ULL);

	VM_CHECK(vm_address_space_query_page(&space_a, VM_USER_A, &mapping));
	VM_CHECK(mapping.physical_address == page_a && mapping.protection == VM_USER_PROTECTION_READ_WRITE);
	VM_CHECK(vm_address_space_query_page(&space_b, VM_USER_A, &mapping) && mapping.physical_address == page_b);

	/* A kernel page allocated *after* both spaces exist is visible in both: shared kernel half. */
	void *shared;

	VM_CHECK(vm_kern_allocate(PMM_PAGE_SIZE, VMM_PROTECTION_READ_WRITE, &shared));
	VM_CHECK(vm_fill_and_verify((uintptr_t)shared, 1024U, 0x5A5EDU));

	/* Inactive spaces are not translated. */
	uint64_t translated;

	VM_CHECK(!vm_address_space_translate_read(&space_a, VM_USER_A, &translated));

	VM_CHECK(vm_address_space_activate(&space_a));
	VM_CHECK(vm_address_space_is_active(&space_a) && !vm_address_space_is_active(&space_b));
	VM_CHECK(vm_address_space_current() == &space_a);
	VM_CHECK(pmap_current_directory_physical() == (uint32_t)space_a.root_physical);
	VM_CHECK(vm_address_space_activate(&space_a));
	VM_CHECK(vm_address_space_translate_read(&space_a, VM_USER_A + 0x123ULL, &translated) && translated == page_a + 0x123ULL);
	VM_CHECK(vm_address_space_translate_write(&space_a, VM_USER_A, &translated) && translated == page_a);
	VM_CHECK(!vm_address_space_translate_read(&space_b, VM_USER_A, &translated));
	VM_CHECK(!vm_address_space_translate_read(&space_a, VM_USER_A + PMAP_PAGE_SIZE, &translated));
	VM_CHECK(!vm_address_space_translate_read(&space_a, 0x10ULL, &translated));

	VM_CHECK(vm_probe_write(VM_USER_A, 0xAAAA0001U));
	VM_CHECK(vm_word((uintptr_t)shared)[3] == 0x5A5EDU + 3U * 0x9E3779B1U);

	VM_CHECK(vm_address_space_activate(&space_b));
	VM_CHECK(vm_address_space_is_active(&space_b) && !vm_address_space_is_active(&space_a));
	VM_CHECK(pmap_current_directory_physical() == (uint32_t)space_b.root_physical);
	VM_CHECK(vm_probe_read(VM_USER_A, &value) && value == 0U);
	VM_CHECK(vm_probe_write(VM_USER_A, 0xBBBB0002U));
	VM_CHECK(vm_word((uintptr_t)shared)[3] == 0x5A5EDU + 3U * 0x9E3779B1U);

	/* Independent contents, checked from the outside through the direct map. */
	VM_CHECK(vm_word(page_a + VMM_HIGHER_HALF_BASE)[0] == 0xAAAA0001U);
	VM_CHECK(vm_word(page_b + VMM_HIGHER_HALF_BASE)[0] == 0xBBBB0002U);

	VM_CHECK(vm_address_space_activate(&space_a));
	VM_CHECK(vm_probe_read(VM_USER_A, &value) && value == 0xAAAA0001U);
	VM_CHECK(vm_address_space_activate(&space_b));
	VM_CHECK(vm_probe_read(VM_USER_A, &value) && value == 0xBBBB0002U);

	/* A mapping present in one space only is a fault in the other. */
	VM_CHECK(vm_address_space_map_page(&space_a, VM_USER_FAR, page_c, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(vm_address_space_table_count(&space_a) == 4ULL);
	VM_CHECK(!vm_probe_read(VM_USER_FAR, 0));
	VM_CHECK((g_last_fault_error & VM_FAULT_PRESENT) == 0U);
	VM_CHECK(vm_address_space_activate(&space_a));
	VM_CHECK(vm_probe_write(VM_USER_FAR, 0xFA4U));
	VM_CHECK(vm_address_space_unmap_page(&space_a, VM_USER_FAR));
	VM_CHECK(!vm_probe_read(VM_USER_FAR, 0));
	VM_CHECK(vm_address_space_table_count(&space_a) == 4ULL);

	/* Protections. A read-only user page refuses even ring 0 (CR0.WP). */
	VM_CHECK(vm_address_space_map_page(&space_a, VM_USER_RO, page_c, VM_USER_PROTECTION_READ_ONLY));
	VM_CHECK(vm_address_space_query_page(&space_a, VM_USER_RO, &mapping) && mapping.protection == VM_USER_PROTECTION_READ_ONLY);
	VM_CHECK(vm_address_space_translate_read(&space_a, VM_USER_RO, &translated));
	VM_CHECK(!vm_address_space_translate_write(&space_a, VM_USER_RO, &translated));
	VM_CHECK(vm_probe_read(VM_USER_RO, &value) && value == 0xFA4U);
	VM_CHECK(!vm_probe_write(VM_USER_RO, 1U));
	VM_CHECK((g_last_fault_error & (VM_FAULT_PRESENT | VM_FAULT_WRITE)) == (VM_FAULT_PRESENT | VM_FAULT_WRITE));
	VM_CHECK(vm_word(page_c + VMM_HIGHER_HALF_BASE)[0] == 0xFA4U);

	/* Read-execute is what the loader asks for the entry page; hardware sees read-only. */
	VM_CHECK(vm_address_space_map_page(&space_a, VM_USER_RX, page_c, VM_USER_PROTECTION_READ_EXECUTE));
	VM_CHECK(vm_address_space_query_page(&space_a, VM_USER_RX, &mapping) && mapping.protection == VM_USER_PROTECTION_READ_EXECUTE);
	VM_CHECK(!vm_probe_write(VM_USER_RX, 1U));
	VM_CHECK(vm_address_space_unmap_page(&space_a, VM_USER_RX));

	VM_CHECK(vm_probe_write(VM_USER_A, 0x11223344U));
	VM_CHECK(vm_address_space_unmap_page(&space_a, VM_USER_RO));

	/*
	 * TLB coherence in the active space: touch a page so its translation is
	 * cached, unmap it, and the access must fault; remap another page there
	 * and the new contents show up.
	 */
	VM_CHECK(vm_probe_read(VM_USER_A, &value) && value == 0x11223344U);
	VM_CHECK(vm_address_space_unmap_page(&space_a, VM_USER_A));
	VM_CHECK(!vm_probe_read(VM_USER_A, 0));
	VM_CHECK(vm_word(page_c + VMM_HIGHER_HALF_BASE)[1] == 0U);
	vm_word(page_c + VMM_HIGHER_HALF_BASE)[1] = 0xC0FFEEU;
	VM_CHECK(vm_address_space_map_page(&space_a, VM_USER_A, page_c, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(vm_probe_read(VM_USER_A + 4U, &value) && value == 0xC0FFEEU);
	VM_CHECK(vm_address_space_unmap_page(&space_a, VM_USER_A));
	VM_CHECK(vm_address_space_map_page(&space_a, VM_USER_A, page_a, VM_USER_PROTECTION_READ_ONLY));
	VM_CHECK(!vm_probe_write(VM_USER_A, 5U));
	VM_CHECK(vm_address_space_unmap_page(&space_a, VM_USER_A));
	VM_CHECK(vm_address_space_map_page(&space_a, VM_USER_A, page_a, VM_USER_PROTECTION_READ_WRITE));
	VM_CHECK(vm_probe_write(VM_USER_A, 0x11223344U));
	VM_CHECK(vm_probe_read(VM_USER_A, &value) && value == 0x11223344U);

	/* Three pages in a fresh page table (rw, rw, ro), with user_copy on top. */
	uint64_t copy_pages[3];

	for (uint32_t index = 0U; index < 3U; index++) {
		VM_CHECK(pmm_allocate_page(&copy_pages[index]));
		VM_CHECK(vm_address_space_map_page(&space_a, VM_USER_COPY + index * PMM_PAGE_SIZE, copy_pages[index], index == 2U ? VM_USER_PROTECTION_READ_ONLY : VM_USER_PROTECTION_READ_WRITE));
	}

	static const char message[] = "user_copy across a page boundary";
	char buffer[sizeof(message)];

	VM_CHECK(vm_copy_to_user(VM_USER_COPY + PMM_PAGE_SIZE - 10ULL, message, sizeof(message)));
	VM_CHECK(vm_copy_from_user(buffer, VM_USER_COPY + PMM_PAGE_SIZE - 10ULL, sizeof(message)));
	VM_CHECK(memcmp(buffer, message, sizeof(message)) == 0);
	VM_CHECK(vm_word(copy_pages[0] + VMM_HIGHER_HALF_BASE)[1023] != 0U);
	VM_CHECK(vm_copy_string_from_user(buffer, VM_USER_COPY + PMM_PAGE_SIZE - 10ULL, sizeof(buffer)));
	VM_CHECK(strcmp(buffer, message) == 0);
	VM_CHECK(!vm_copy_string_from_user(buffer, VM_USER_COPY + PMM_PAGE_SIZE - 10ULL, 8ULL));
	VM_CHECK(!vm_copy_to_user(VM_USER_COPY + 2ULL * PMM_PAGE_SIZE, message, 4ULL));
	VM_CHECK(vm_copy_from_user(buffer, VM_USER_COPY + 2ULL * PMM_PAGE_SIZE, 4ULL));
	VM_CHECK(!vm_copy_from_user(buffer, VM_USER_COPY + 3ULL * PMM_PAGE_SIZE, 4ULL));
	VM_CHECK(!vm_copy_to_user(VM_USER_COPY + 3ULL * PMM_PAGE_SIZE - 2ULL, message, 4ULL));

	/* Kernel-only memory stays out of reach of the user-address API. */
	VM_CHECK(!vm_address_space_query_page(&space_a, VM_KERN_BASE, &mapping));
	VM_CHECK(!vm_address_space_query_page(&space_a, 0xC0100000ULL, &mapping));
	VM_CHECK(!vm_address_space_query_page(&space_a, 0ULL, &mapping));

	/* Back to the kernel-only regime. */
	VM_CHECK(vm_address_space_deactivate());
	VM_CHECK(vm_address_space_current() == 0 && !vm_address_space_is_active(&space_a));
	VM_CHECK(pmap_current_directory_physical() == pmap_kernel_directory_physical());
	VM_CHECK(!vm_probe_read(VM_USER_A, 0));
	VM_CHECK(vm_word((uintptr_t)shared)[3] == 0x5A5EDU + 3U * 0x9E3779B1U);
	VM_CHECK(vm_kern_free(shared, PMM_PAGE_SIZE));

	/* Teardown returns every page: data pages, page tables, directories. */
	VM_CHECK(!vm_address_space_destroy(&(vm_address_space_t){ 0 }));

	uint64_t released = vm_address_space_release_pages(&space_a);

	VM_CHECK(released == 4ULL);
	VM_CHECK(!vm_address_space_query_page(&space_a, VM_USER_A, &mapping));
	VM_CHECK(vm_address_space_destroy(&space_a));
	VM_CHECK(space_a.root == 0 && space_a.root_physical == 0ULL);

	VM_CHECK(vm_address_space_activate(&space_b));
	VM_CHECK(vm_address_space_destroy(&space_b));
	VM_CHECK(vm_address_space_current() == 0);
	VM_CHECK(pmap_current_directory_physical() == pmap_kernel_directory_physical());

	/* page_b was owned by space_b's mapping, released above nothing; free the loose pages. */
	VM_CHECK(pmm_free_page(page_b));
	VM_CHECK(pmm_free_page(page_c));

	VM_CHECK(pmm_get_free_page_count() == free_before);

	kprintf("vm_test_address_spaces: passed (isolation, shared kernel half, CR3 switching, protections, TLB, teardown)\n");
	return true;
}

/* --------------------------------------------------------------------------
 * heap
 * ------------------------------------------------------------------------ */

static bool vm_test_heap(void)
{
	void *probe = kmalloc(1U);

	if (probe != 0) VM_CHECK(kfree(probe));
	else VM_CHECK(heap_init());

	uint64_t allocations_before = heap_get_allocation_count();
	uint64_t pages_before = heap_get_page_count();
	uint64_t large_before = heap_get_large_allocation_count();
	uint64_t free_before = pmm_get_free_page_count();

	void *blocks[8];
	size_t sizes[8] = { 1U, 24U, 100U, 1000U, 2000U, 4000U, 40000U, 300000U };

	for (uint32_t index = 0U; index < 8U; index++) {
		blocks[index] = kmalloc(sizes[index]);
		VM_CHECK(blocks[index] != 0);
		VM_CHECK(((uintptr_t)blocks[index] & 15U) == 0U);
	}

	/* Distinct, non-overlapping, intact. */
	for (uint32_t index = 0U; index < 8U; index++) memset(blocks[index], (int)(0xA0U + index), sizes[index]);

	for (uint32_t index = 0U; index < 8U; index++) {
		const uint8_t *bytes = (const uint8_t *)blocks[index];

		for (size_t offset = 0U; offset < sizes[index]; offset += 61U) VM_CHECK(bytes[offset] == 0xA0U + index);
		VM_CHECK(bytes[sizes[index] - 1U] == 0xA0U + index);
	}

	VM_CHECK(heap_get_allocation_count() == allocations_before + 8ULL);
	VM_CHECK(heap_get_large_allocation_count() == large_before + 2ULL);
	VM_CHECK(vm_kern_contains(blocks[7]) && vm_kern_contains(blocks[6]));
	VM_CHECK(!vm_kern_contains(blocks[3]));

	/* kcalloc returns zeroes even over memory that was just dirtied. */
	VM_CHECK(kfree(blocks[3]));
	uint8_t *zeroed = kcalloc(25U, 40U);

	VM_CHECK(zeroed != 0);

	for (uint32_t index = 0U; index < 1000U; index++) VM_CHECK(zeroed[index] == 0U);

	VM_CHECK(kfree(zeroed));
	VM_CHECK(kcalloc(0U, 4U) == 0 && kcalloc(4U, 0U) == 0 && kmalloc(0U) == 0);
	VM_CHECK(kcalloc((size_t)-1, 16U) == 0);

	/* Many small blocks, freed in an order that exercises coalescing. */
	void *many[200];

	for (uint32_t index = 0U; index < 200U; index++) {
		many[index] = kmalloc(64U);
		VM_CHECK(many[index] != 0);
		*(uint32_t *)many[index] = index;
	}

	for (uint32_t index = 0U; index < 200U; index++) VM_CHECK(*(uint32_t *)many[index] == index);
	for (uint32_t index = 0U; index < 200U; index += 2U) VM_CHECK(kfree(many[index]));
	for (uint32_t index = 1U; index < 200U; index += 2U) VM_CHECK(kfree(many[index]));

	for (uint32_t index = 0U; index < 8U; index++) {
		if (index != 3U) VM_CHECK(kfree(blocks[index]));
	}

	VM_CHECK(!kfree(0));

	VM_CHECK(heap_get_allocation_count() == allocations_before);
	VM_CHECK(heap_get_large_allocation_count() == large_before);
	VM_CHECK(heap_get_page_count() == pages_before);
	VM_CHECK(pmm_get_free_page_count() == free_before);

	kprintf("vm_test_heap: passed (small, page-sized, large, kcalloc, coalescing)\n");
	return true;
}

/* --------------------------------------------------------------------------
 * deliberate faults, for the trap layer's report
 * ------------------------------------------------------------------------ */

static void vm_selftest_deliberate_fault(const char *name)
{
	kprintf("i386_init_vm_selftest: raising deliberate fault \"%s\"\n", name);

	if (strcmp(name, "write-ro") == 0) {
		void *page;

		if (!vm_kern_allocate(PMM_PAGE_SIZE, VMM_PROTECTION_READ_ONLY, &page)) return;
		*(volatile uint32_t *)page = 1U;
	} else if (strcmp(name, "write-text") == 0) {
		*(volatile uint32_t *)(uintptr_t)__text_start = 1U;
	} else if (strcmp(name, "null") == 0) {
		volatile uint32_t sink = *(volatile uint32_t *)0;

		(void)sink;
	} else if (strcmp(name, "unmapped") == 0) {
		volatile uint32_t sink = *(volatile uint32_t *)(uintptr_t)VM_ARENA_SPARE;

		(void)sink;
	} else if (strcmp(name, "user-null") == 0 || strcmp(name, "user-ro") == 0) {
		static vm_address_space_t space;
		uint64_t page;

		if (!vm_address_space_create(&space) || !pmm_allocate_page(&page)) return;
		if (!vm_address_space_map_page(&space, VM_USER_RO, page, VM_USER_PROTECTION_READ_ONLY)) return;
		if (!vm_address_space_activate(&space)) return;

		if (strcmp(name, "user-null") == 0) {
			volatile uint32_t sink = *(volatile uint32_t *)0x10;

			(void)sink;
		} else {
			*(volatile uint32_t *)(uintptr_t)VM_USER_RO = 1U;
		}
	} else {
		kprintf("i386_init_vm_selftest: unknown vm-fault \"%s\"\n", name);
	}
}

bool i386_init_vm_selftest(const i386_boot_info_t *boot)
{
	platform_t platform;

	memset(&platform, 0, sizeof(platform));

	if (boot == 0 || boot->multiboot == 0 || !i386_memory_map_load(&platform, boot->multiboot)) {
		kputln("i386_init_vm_selftest: no memory map to test against");
		return false;
	}

	if (!vm_test_pmm(boot)) return false;
	if (!vm_test_direct_map(&platform)) return false;
	if (!vm_test_vm_kern()) return false;
	if (!vm_test_kernel_mappings()) return false;

	/* The heap backs vm_shm, vm_map and the address-space tests' bookkeeping. */
	if (!vm_test_heap()) return false;
	if (!vm_test_address_spaces()) return false;

	if (!vm_shm_self_test()) {
		kputln("i386_init_vm_selftest: vm_shm self-test failed");
		return false;
	}

	if (!vm_map_self_test()) {
		kputln("i386_init_vm_selftest: vm_map self-test failed");
		return false;
	}

	if (pmap_current_directory_physical() != pmap_kernel_directory_physical()) {
		kputln("i386_init_vm_selftest: a test left a user address space active");
		return false;
	}

	kprintf(
		"i386_init_vm_selftest: all checks passed, %llu free page(s), %llu used\n",
		pmm_get_free_page_count(),
		pmm_get_used_page_count()
	);

	char fault[32];

	if (i386_boot_arg("vm-fault", fault, sizeof(fault))) {
		vm_selftest_deliberate_fault(fault);

		/* Only reached if the fault did not happen. */
		kputln("i386_init_vm_selftest: deliberate fault did not fault");
		return false;
	}

	return true;
}
