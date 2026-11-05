/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/x86_64/memory_map.c
 *
 * Converts the UEFI handoff map into page-aligned RAM ranges understood by
 * the physical memory manager.
 */

#include <platform/x86_64/memory_map.h>

#include <kern/console/console.h>
#include <kern/machine/vm_param.h>

#include <stdint.h>
#include <string.h>

#define X86_MEMORY_MAP_MINIMUM 0x00100000ULL
#define X86_MEMORY_MAP_USABLE 1U

/*
 * Routine:     x86_platform_memory_map_load
 * Purpose:
 *              Validate and compact usable UEFI entries into sorted, aligned
 *              RAM ranges, dropping memory outside the current direct map.
 */
bool
x86_platform_memory_map_load(const struct nxu_boot_x86_64 *boot, platform_t *platform)
{
	if (boot == 0 || platform == 0 || boot->memory_map == 0ULL || boot->memory_map_size == 0ULL ||
		boot->memory_descriptor_size != sizeof(struct nxu_boot_memory_map_entry) ||
		boot->memory_map_size % boot->memory_descriptor_size != 0ULL ||
		boot->memory_map > UINT64_MAX - boot->memory_map_size) {
		kprintf("x86_platform_memory_map_load: invalid handoff fields\n");
		return false;
	}

	memset(platform, 0, sizeof(*platform));

	const struct nxu_boot_memory_map_entry *entries = (const void *)(uintptr_t)boot->memory_map;
	uint64_t entry_count = boot->memory_map_size / boot->memory_descriptor_size;

	for (uint64_t index = 0ULL; index < entry_count; index++) {
		const struct nxu_boot_memory_map_entry *entry = &entries[index];

		if (entry->type != X86_MEMORY_MAP_USABLE || entry->length == 0ULL || entry->address > UINT64_MAX - entry->length) {
			continue;
		}

		uint64_t base = entry->address;
		uint64_t end = base + entry->length;

		if (base < X86_MEMORY_MAP_MINIMUM) {
			base = X86_MEMORY_MAP_MINIMUM;
		}

		if (end > VM_DIRECT_MAP_SIZE) {
			end = VM_DIRECT_MAP_SIZE;
		}

		base = (base + 4095ULL) & ~4095ULL;
		end &= ~4095ULL;

		if (end <= base) {
			continue;
		}

		if (platform->memory_region_count != 0U) {
			platform_region_t *previous = &platform->memory_regions[platform->memory_region_count - 1U];
			uint64_t previous_end = previous->base + previous->size;

			if (base < previous_end) {
				kprintf("x86_platform_memory_map_load: overlapping usable ranges %u: 0x%llx < 0x%llx\n",
						(unsigned)index, (unsigned long long)base, (unsigned long long)previous_end);
				return false;
			}

			if (base == previous_end) {
				previous->size += end - base;
				continue;
			}
		}

		if (platform->memory_region_count >= PLATFORM_MAX_MEMORY_REGIONS) {
			continue;
		}

		platform_region_t *region = &platform->memory_regions[platform->memory_region_count++];
		region->base = base;
		region->size = end - base;
	}

	if (platform->memory_region_count == 0U) {
		kprintf("x86_platform_memory_map_load: no usable ranges below 1 GiB\n");
		return false;
	}

	return true;
}
