/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/memory_map.h
 *
 * Multiboot memory-map parser. Turns the loader's E820-style map into the
 * platform_t memory region list the physical memory manager initialises from.
 *
 * What ends up in platform->memory_regions:
 *
 *   - only RAM the loader reports as available (type 1), with every other
 *     range (reserved, ACPI, NVS, bad) carved out even if the map overlaps;
 *   - clipped to physical [I386_MEMORY_MIN, VM_DIRECT_MAP_SIZE): the first
 *     MiB belongs to the firmware and boot structures, and RAM above the
 *     direct map cannot be used (there is no highmem);
 *   - page aligned, sorted by address, adjacent runs merged.
 *
 * If the loader gave no memory map, mem_upper (KiB above 1 MiB) stands in
 * for a single region starting at 1 MiB.
 */

#ifndef NXU_KERN_I386_MEMORY_MAP_H
#define NXU_KERN_I386_MEMORY_MAP_H

#include <kern/i386/multiboot.h>

#include <platform/platform.h>

#include <stdbool.h>
#include <stdint.h>

#define MULTIBOOT_INFO_MODS (1U << 3U)

#define MULTIBOOT_MEMORY_AVAILABLE 1U

#define I386_MEMORY_MIN 0x00100000ULL

/* One entry of the Multiboot memory map. `size` excludes the size field. */
typedef struct __attribute__((packed)) {
	uint32_t size;
	uint64_t base;
	uint64_t length;
	uint32_t type;
} multiboot_mmap_entry_t;

typedef struct __attribute__((packed)) {
	uint32_t mod_start;
	uint32_t mod_end;
	uint32_t string;
	uint32_t reserved;
} multiboot_module_t;

_Static_assert(sizeof(multiboot_mmap_entry_t) == 24U, "multiboot mmap entry layout");
_Static_assert(sizeof(multiboot_module_t) == 16U, "multiboot module layout");

/*
 * Fill platform->memory_regions / memory_region_count (nothing else in
 * platform is touched) from the Multiboot information. Returns false, with
 * the region count 0, if there is no usable RAM at all.
 *
 * Addresses inside mbi (mmap_addr, ...) may be physical or, after
 * i386_boot_relocate(), direct-map virtual addresses; both are accepted.
 */
bool i386_memory_map_load(platform_t *platform, const multiboot_info_t *mbi);

/* Print the regions in platform, one line each. */
void i386_memory_map_dump(const platform_t *platform);

/*
 * Physical extent of the boot structures a bootloader left behind (the info
 * structure, command line, memory map, module list and the modules
 * themselves). The VM layer keeps these out of the page allocator: the
 * command line in particular stays in use for the whole boot.
 *
 * Calls report(base, size, context) once per range and returns the number of
 * ranges reported. Sizes are conservative upper bounds, not page aligned.
 * mbi may be the direct-map alias of the structure or its physical address.
 */
typedef void (*i386_boot_range_fn)(uint64_t base, uint64_t size, void *context);

uint32_t i386_memory_map_boot_ranges(const multiboot_info_t *mbi, i386_boot_range_fn report, void *context);

/* Direct-map pointer for a physical (or already direct-map) bootloader address. */
const void *i386_memory_map_pointer(uint32_t address);

#endif
