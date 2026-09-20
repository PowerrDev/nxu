/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/memory_map.c
 *
 * See memory_map.h.
 */

#include <kern/i386/memory_map.h>

#include <kern/machine/vm_param.h>

#include <kern/console/console.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MEMORY_MAP_PAGE 4096ULL

/* Scratch capacity while carving reserved ranges out of usable ones. */
#define MEMORY_MAP_WORK_MAX 64U

/* The base multiboot_info_t is 88 bytes; the full spec structure 116. */
#define MULTIBOOT_INFO_SPAN 128ULL
#define MULTIBOOT_STRING_MAX 4096U

typedef struct {
	uint64_t base;
	uint64_t end;
} memory_range_t;

const void *i386_memory_map_pointer(uint32_t address)
{
	if (address >= VMM_HIGHER_HALF_BASE) {
		return address < VMM_HIGHER_HALF_BASE + VM_DIRECT_MAP_SIZE ? (const void *)(uintptr_t)address : 0;
	}

	if (address >= VM_DIRECT_MAP_SIZE) return 0;
	return (const void *)(uintptr_t)(address + VMM_HIGHER_HALF_BASE);
}

static uint64_t memory_map_physical(uint32_t address)
{
	return address >= VMM_HIGHER_HALF_BASE ? (uint64_t)(address - VMM_HIGHER_HALF_BASE) : (uint64_t)address;
}

static uint64_t memory_map_align_up(uint64_t value)
{
	return (value + (MEMORY_MAP_PAGE - 1ULL)) & ~(MEMORY_MAP_PAGE - 1ULL);
}

static uint64_t memory_map_align_down(uint64_t value)
{
	return value & ~(MEMORY_MAP_PAGE - 1ULL);
}

/*
 * Add [base, end) to list after clipping it to the range the kernel can use
 * and rounding inward to whole pages. Drops what is left empty or does not fit.
 */
static void memory_map_add_usable(memory_range_t *list, uint32_t *count, uint64_t base, uint64_t end)
{
	if (end <= base) return;

	if (base < I386_MEMORY_MIN) base = I386_MEMORY_MIN;
	if (end > VM_DIRECT_MAP_SIZE) end = VM_DIRECT_MAP_SIZE;

	base = memory_map_align_up(base);
	end = memory_map_align_down(end);

	if (end <= base || *count >= MEMORY_MAP_WORK_MAX) return;

	list[*count].base = base;
	list[*count].end = end;
	(*count)++;
}

/* Remove [hole_base, hole_end) from every range in list, splitting as needed. */
static void memory_map_carve(memory_range_t *list, uint32_t *count, uint64_t hole_base, uint64_t hole_end)
{
	memory_range_t result[MEMORY_MAP_WORK_MAX];
	uint32_t result_count = 0U;

	if (hole_end <= hole_base) return;

	for (uint32_t index = 0U; index < *count; index++) {
		const memory_range_t *range = &list[index];

		if (hole_end <= range->base || hole_base >= range->end) {
			if (result_count < MEMORY_MAP_WORK_MAX) result[result_count++] = *range;
			continue;
		}

		if (hole_base > range->base && result_count < MEMORY_MAP_WORK_MAX) {
			result[result_count].base = range->base;
			result[result_count].end = hole_base;
			result_count++;
		}

		if (hole_end < range->end && result_count < MEMORY_MAP_WORK_MAX) {
			result[result_count].base = hole_end;
			result[result_count].end = range->end;
			result_count++;
		}
	}

	for (uint32_t index = 0U; index < result_count; index++) list[index] = result[index];
	*count = result_count;
}

static void memory_map_sort(memory_range_t *list, uint32_t count)
{
	for (uint32_t index = 1U; index < count; index++) {
		memory_range_t value = list[index];
		uint32_t position = index;

		while (position > 0U && list[position - 1U].base > value.base) {
			list[position] = list[position - 1U];
			position--;
		}

		list[position] = value;
	}
}

static uint32_t memory_map_merge(memory_range_t *list, uint32_t count)
{
	uint32_t out = 0U;

	for (uint32_t index = 0U; index < count; index++) {
		if (out != 0U && list[index].base <= list[out - 1U].end) {
			if (list[index].end > list[out - 1U].end) list[out - 1U].end = list[index].end;
			continue;
		}

		list[out++] = list[index];
	}

	return out;
}

bool i386_memory_map_load(platform_t *platform, const multiboot_info_t *mbi)
{
	memory_range_t usable[MEMORY_MAP_WORK_MAX];
	uint32_t count = 0U;

	if (platform == 0) return false;

	platform->memory_region_count = 0U;

	if (mbi == 0) return false;

	const multiboot_mmap_entry_t *first = 0;
	uint32_t map_length = 0U;

	if ((mbi->flags & MULTIBOOT_INFO_MMAP) != 0U && mbi->mmap_length != 0U) {
		first = (const multiboot_mmap_entry_t *)i386_memory_map_pointer(mbi->mmap_addr);
		map_length = mbi->mmap_length;
	}

	if (first != 0) {
		const uint8_t *cursor = (const uint8_t *)first;
		const uint8_t *limit = cursor + map_length;

		/* Pass one: usable RAM. Pass two: everything else, carved back out. */
		for (const uint8_t *entry_pointer = cursor; entry_pointer + sizeof(multiboot_mmap_entry_t) <= limit;) {
			const multiboot_mmap_entry_t *entry = (const multiboot_mmap_entry_t *)entry_pointer;

			if (entry->type == MULTIBOOT_MEMORY_AVAILABLE && entry->length <= UINT64_MAX - entry->base) {
				memory_map_add_usable(usable, &count, entry->base, entry->base + entry->length);
			}

			entry_pointer += entry->size + sizeof(uint32_t);
		}

		for (const uint8_t *entry_pointer = cursor; entry_pointer + sizeof(multiboot_mmap_entry_t) <= limit;) {
			const multiboot_mmap_entry_t *entry = (const multiboot_mmap_entry_t *)entry_pointer;

			if (entry->type != MULTIBOOT_MEMORY_AVAILABLE) {
				uint64_t end = entry->length > UINT64_MAX - entry->base ? UINT64_MAX : entry->base + entry->length;

				memory_map_carve(usable, &count, memory_map_align_down(entry->base), end == UINT64_MAX ? end : memory_map_align_up(end));
			}

			entry_pointer += entry->size + sizeof(uint32_t);
		}
	} else if ((mbi->flags & MULTIBOOT_INFO_MEMORY) != 0U && mbi->mem_upper != 0U) {
		memory_map_add_usable(usable, &count, 0x100000ULL, 0x100000ULL + (uint64_t)mbi->mem_upper * 1024ULL);
	}

	memory_map_sort(usable, count);
	count = memory_map_merge(usable, count);

	/* platform_t holds a handful of regions: keep the largest ones. */
	while (count > PLATFORM_MAX_MEMORY_REGIONS) {
		uint32_t smallest = 0U;

		for (uint32_t index = 1U; index < count; index++) {
			if (usable[index].end - usable[index].base < usable[smallest].end - usable[smallest].base) smallest = index;
		}

		for (uint32_t index = smallest + 1U; index < count; index++) usable[index - 1U] = usable[index];
		count--;
	}

	for (uint32_t index = 0U; index < count; index++) {
		platform->memory_regions[index].base = usable[index].base;
		platform->memory_regions[index].size = usable[index].end - usable[index].base;
	}

	platform->memory_region_count = count;

	return count != 0U;
}

void i386_memory_map_dump(const platform_t *platform)
{
	uint64_t total = 0ULL;

	for (uint32_t index = 0U; index < platform->memory_region_count; index++) {
		const platform_region_t *region = &platform->memory_regions[index];

		kprintf(
			"i386_memory_map_dump: region %u: 0x%llx-0x%llx (%llu KiB)\n",
			index,
			region->base,
			region->base + region->size,
			region->size / 1024ULL
		);
		total += region->size;
	}

	kprintf("i386_memory_map_dump: %u region(s), %llu KiB usable\n", platform->memory_region_count, total / 1024ULL);
}

static uint32_t memory_map_string_length(const char *string)
{
	uint32_t length = 0U;

	while (length < MULTIBOOT_STRING_MAX && string[length] != '\0') length++;
	return length + 1U;
}

uint32_t i386_memory_map_boot_ranges(const multiboot_info_t *mbi, i386_boot_range_fn report, void *context)
{
	uint32_t reported = 0U;

	if (mbi == 0 || report == 0) return 0U;

	report(memory_map_physical((uint32_t)(uintptr_t)mbi), MULTIBOOT_INFO_SPAN, context);
	reported++;

	if ((mbi->flags & MULTIBOOT_INFO_CMDLINE) != 0U && mbi->cmdline != 0U) {
		const char *cmdline = (const char *)i386_memory_map_pointer(mbi->cmdline);

		if (cmdline != 0) {
			report(memory_map_physical(mbi->cmdline), memory_map_string_length(cmdline), context);
			reported++;
		}
	}

	if ((mbi->flags & MULTIBOOT_INFO_MMAP) != 0U && mbi->mmap_length != 0U) {
		report(memory_map_physical(mbi->mmap_addr), mbi->mmap_length, context);
		reported++;
	}

	if ((mbi->flags & MULTIBOOT_INFO_MODS) != 0U && mbi->mods_count != 0U) {
		const multiboot_module_t *modules = (const multiboot_module_t *)i386_memory_map_pointer(mbi->mods_addr);

		if (modules != 0) {
			report(memory_map_physical(mbi->mods_addr), (uint64_t)mbi->mods_count * sizeof(multiboot_module_t), context);
			reported++;

			for (uint32_t index = 0U; index < mbi->mods_count; index++) {
				if (modules[index].mod_end > modules[index].mod_start) {
					report(modules[index].mod_start, (uint64_t)modules[index].mod_end - modules[index].mod_start, context);
					reported++;
				}

				if (modules[index].string != 0U) {
					const char *string = (const char *)i386_memory_map_pointer(modules[index].string);

					if (string != 0) {
						report(memory_map_physical(modules[index].string), memory_map_string_length(string), context);
						reported++;
					}
				}
			}
		}
	}

	return reported;
}
