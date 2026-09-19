/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/loader/elf_format.h
 *
 * The ELF executable format, separated from what the loader does with it.
 * Everything here is pure: it decodes and validates bytes and needs no VFS,
 * no VM and no scheduler, so the checks can be exercised on their own (see
 * elf_selftest.c) and shared between architectures.
 *
 * Two on-disk flavours exist and each build accepts exactly one, chosen by
 * the compiler's target:
 *
 *   arm64   ELF64, little-endian, EM_AARCH64  (the layouts the loader has
 *           always read directly into memory)
 *   i386    ELF32, little-endian, EM_386
 *
 * The loader's working representation is the wide one, loader_header_t and
 * loader_segment_t (64-bit fields). On arm64 it *is* the on-disk layout; on
 * i386 the 32-bit on-disk records are widened into it by
 * loader_header_widen() / loader_segment_widen(), so every check below is
 * written once, in 64-bit arithmetic that cannot overflow on 32-bit inputs.
 */

#ifndef NXU_KERN_LOADER_ELF_FORMAT_H
#define NXU_KERN_LOADER_ELF_FORMAT_H

#include <mach/machine/vm_param.h>

#include <stdbool.h>
#include <stdint.h>

#define ELF_IDENT_SIZE 16U
#define ELF_CLASS_32 1U
#define ELF_CLASS_64 2U
#define ELF_DATA_LSB 1U
#define ELF_VERSION_CURRENT 1U
#define ELF_TYPE_EXEC 2U
#define ELF_MACHINE_386 3U
#define ELF_MACHINE_AARCH64 183U
#define ELF_PT_LOAD 1U
#define ELF_PF_X 0x1U
#define ELF_PF_W 0x2U
#define ELF_PROGRAM_HEADER_MAX 16U

#define LOADER_PAGE_SIZE 4096ULL
#define LOADER_USER_STACK_PAGES 4ULL
#define LOADER_USER_STACK_TOP VM_USER_STACK_TOP
#define LOADER_USER_STACK_BASE (LOADER_USER_STACK_TOP - LOADER_USER_STACK_PAGES * LOADER_PAGE_SIZE)

typedef struct {
	uint8_t ident[ELF_IDENT_SIZE];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint64_t entry;
	uint64_t program_header_offset;
	uint64_t section_header_offset;
	uint32_t flags;
	uint16_t header_size;
	uint16_t program_header_entry_size;
	uint16_t program_header_count;
	uint16_t section_header_entry_size;
	uint16_t section_header_count;
	uint16_t section_header_string_index;
} elf64_header_t;

typedef struct {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t virtual_address;
	uint64_t physical_address;
	uint64_t file_size;
	uint64_t memory_size;
	uint64_t alignment;
} elf64_program_header_t;

typedef struct {
	uint8_t ident[ELF_IDENT_SIZE];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint32_t entry;
	uint32_t program_header_offset;
	uint32_t section_header_offset;
	uint32_t flags;
	uint16_t header_size;
	uint16_t program_header_entry_size;
	uint16_t program_header_count;
	uint16_t section_header_entry_size;
	uint16_t section_header_count;
	uint16_t section_header_string_index;
} elf32_header_t;

/* Note the field order: unlike ELF64, ELF32 puts flags after memory_size. */
typedef struct {
	uint32_t type;
	uint32_t offset;
	uint32_t virtual_address;
	uint32_t physical_address;
	uint32_t file_size;
	uint32_t memory_size;
	uint32_t flags;
	uint32_t alignment;
} elf32_program_header_t;

_Static_assert(sizeof(elf64_header_t) == 64U, "ELF64 header layout mismatch");
_Static_assert(sizeof(elf64_program_header_t) == 56U, "ELF64 program header layout mismatch");
_Static_assert(sizeof(elf32_header_t) == 52U, "ELF32 header layout mismatch");
_Static_assert(sizeof(elf32_program_header_t) == 32U, "ELF32 program header layout mismatch");

/* The wide working representation; see the file comment. */
typedef elf64_header_t loader_header_t;
typedef elf64_program_header_t loader_segment_t;

#if defined(__i386__)

#define LOADER_ELF_WIDEN 1
#define LOADER_ELF_CLASS ELF_CLASS_32
#define LOADER_ELF_MACHINE ELF_MACHINE_386
#define LOADER_ELF_HEADER_SIZE sizeof(elf32_header_t)
#define LOADER_ELF_PROGRAM_HEADER_SIZE sizeof(elf32_program_header_t)

typedef elf32_header_t loader_disk_header_t;
typedef elf32_program_header_t loader_disk_segment_t;

static inline void
loader_header_widen(const loader_disk_header_t *disk, loader_header_t *header)
{
	for (uint32_t index = 0U; index < ELF_IDENT_SIZE; index++) header->ident[index] = disk->ident[index];
	header->type = disk->type;
	header->machine = disk->machine;
	header->version = disk->version;
	header->entry = disk->entry;
	header->program_header_offset = disk->program_header_offset;
	header->section_header_offset = disk->section_header_offset;
	header->flags = disk->flags;
	header->header_size = disk->header_size;
	header->program_header_entry_size = disk->program_header_entry_size;
	header->program_header_count = disk->program_header_count;
	header->section_header_entry_size = disk->section_header_entry_size;
	header->section_header_count = disk->section_header_count;
	header->section_header_string_index = disk->section_header_string_index;
}

static inline void
loader_segment_widen(const loader_disk_segment_t *disk, loader_segment_t *segment)
{
	segment->type = disk->type;
	segment->flags = disk->flags;
	segment->offset = disk->offset;
	segment->virtual_address = disk->virtual_address;
	segment->physical_address = disk->physical_address;
	segment->file_size = disk->file_size;
	segment->memory_size = disk->memory_size;
	segment->alignment = disk->alignment;
}

#else

#define LOADER_ELF_WIDEN 0
#define LOADER_ELF_CLASS ELF_CLASS_64
#define LOADER_ELF_MACHINE ELF_MACHINE_AARCH64
#define LOADER_ELF_HEADER_SIZE sizeof(elf64_header_t)
#define LOADER_ELF_PROGRAM_HEADER_SIZE sizeof(elf64_program_header_t)

typedef elf64_header_t loader_disk_header_t;
typedef elf64_program_header_t loader_disk_segment_t;

#endif

static inline bool
loader_add_overflow(uint64_t left, uint64_t right, uint64_t *result)
{
	if (result == 0 || left > UINT64_MAX - right) return true;
	*result = left + right;
	return false;
}

static inline bool
loader_align_up(uint64_t value, uint64_t alignment, uint64_t *result)
{
	if (result == 0 || alignment == 0ULL || (alignment & (alignment - 1ULL)) != 0ULL) return false;
	if (value > UINT64_MAX - (alignment - 1ULL)) return false;
	*result = (value + alignment - 1ULL) & ~(alignment - 1ULL);
	return true;
}

static inline bool
loader_header_valid(const loader_header_t *header)
{
	if (header == 0) return false;
	if (header->ident[0] != 0x7FU || header->ident[1] != 'E' || header->ident[2] != 'L' || header->ident[3] != 'F') return false;
	if (header->ident[4] != LOADER_ELF_CLASS || header->ident[5] != ELF_DATA_LSB || header->ident[6] != ELF_VERSION_CURRENT) return false;
	if (header->type != ELF_TYPE_EXEC || header->machine != LOADER_ELF_MACHINE || header->version != ELF_VERSION_CURRENT) return false;
	if (header->header_size != LOADER_ELF_HEADER_SIZE || header->program_header_entry_size != LOADER_ELF_PROGRAM_HEADER_SIZE) return false;
	if (header->program_header_count == 0U || header->program_header_count > ELF_PROGRAM_HEADER_MAX) return false;
	return header->entry != 0ULL;
}

/* A writable segment must not also be executable (no W+X anywhere in a process). */
static inline bool
loader_segment_flags_valid(uint32_t flags)
{
	return (flags & ELF_PF_W) == 0U || (flags & ELF_PF_X) == 0U;
}

/* The segment's bytes must lie inside a file of file_size bytes. */
static inline bool
loader_segment_in_file(const loader_segment_t *segment, uint64_t file_size)
{
	uint64_t file_end;
	return !loader_add_overflow(segment->offset, segment->file_size, &file_end) && file_end <= file_size;
}

/*
 * Everything that can be decided about one PT_LOAD segment from the header
 * alone: it fills at least what the file supplies, keeps its page offset
 * congruent between file and memory, does not wrap, lies inside
 * [one page, stack base), and is not W+X.
 */
static inline bool
loader_segment_valid(const loader_segment_t *segment)
{
	if (segment->memory_size < segment->file_size || segment->memory_size == 0ULL) return false;
	if ((segment->virtual_address & (LOADER_PAGE_SIZE - 1ULL)) != (segment->offset & (LOADER_PAGE_SIZE - 1ULL))) return false;

	uint64_t segment_end;
	if (loader_add_overflow(segment->virtual_address, segment->memory_size, &segment_end)) return false;
	if (segment->virtual_address < LOADER_PAGE_SIZE || segment_end > LOADER_USER_STACK_BASE) return false;

	uint64_t page_end;
	if (!loader_align_up(segment_end, LOADER_PAGE_SIZE, &page_end)) return false;

	return loader_segment_flags_valid(segment->flags);
}

/*
 * The program header table must sit inside the file. Returns the table's size
 * in bytes through table_size (in units of the on-disk record).
 */
static inline bool
loader_program_table_valid(const loader_header_t *header, uint64_t file_size, uint64_t *table_size)
{
	uint64_t size = (uint64_t)header->program_header_count * LOADER_ELF_PROGRAM_HEADER_SIZE;
	uint64_t end;
	if (loader_add_overflow(header->program_header_offset, size, &end) || end > file_size) return false;
	*table_size = size;
	return true;
}

/*
 * What loader_image_inspect() learned about an image it accepted.
 * image_low / image_high are the page-aligned bounds of the loaded segments.
 */
typedef struct {
	uint64_t entry;
	uint32_t segment_count;
	uint64_t image_low;
	uint64_t image_high;
} loader_image_info_t;

/*
 * Run every header-only check the loader applies to an executable held
 * entirely in memory, without touching the VFS or the VM, and report the
 * first thing wrong with it (a short static string) or 0 if it is loadable.
 *
 * Beyond the per-segment checks above this also rejects two PT_LOAD
 * segments that share a page (loader_map_segment refuses the second one when
 * it finds the page already mapped) and an entry point that is not inside an
 * executable, non-writable segment (loader_spawn queries the entry page after
 * mapping and requires it to be read+execute).
 */
static inline const char *
loader_image_inspect(const void *image, uint64_t size, loader_image_info_t *info)
{
	const uint8_t *bytes = image;
	loader_disk_header_t disk_header;
	loader_header_t header;

	if (image == 0 || info == 0) return "no image";
	if (size < sizeof(disk_header)) return "truncated header";

	for (uint32_t index = 0U; index < sizeof(disk_header); index++) ((uint8_t *)&disk_header)[index] = bytes[index];
#if LOADER_ELF_WIDEN
	loader_header_widen(&disk_header, &header);
#else
	header = disk_header;
#endif
	if (!loader_header_valid(&header)) return "bad header";

	uint64_t table_size;
	if (!loader_program_table_valid(&header, size, &table_size)) return "program table outside file";

	loader_segment_t segments[ELF_PROGRAM_HEADER_MAX];
	for (uint32_t index = 0U; index < header.program_header_count; index++) {
		loader_disk_segment_t disk_segment;
		const uint8_t *record = bytes + header.program_header_offset + (uint64_t)index * sizeof(disk_segment);

		for (uint32_t byte = 0U; byte < sizeof(disk_segment); byte++) ((uint8_t *)&disk_segment)[byte] = record[byte];
#if LOADER_ELF_WIDEN
		loader_segment_widen(&disk_segment, &segments[index]);
#else
		segments[index] = disk_segment;
#endif
	}

	info->entry = header.entry;
	info->segment_count = 0U;
	info->image_low = UINT64_MAX;
	info->image_high = 0ULL;

	bool entry_ok = false;
	uint64_t entry_page = header.entry & ~(LOADER_PAGE_SIZE - 1ULL);

	for (uint32_t index = 0U; index < header.program_header_count; index++) {
		const loader_segment_t *segment = &segments[index];

		if (segment->type != ELF_PT_LOAD) continue;
		if (!loader_segment_in_file(segment, size)) return "segment outside file";
		if (!loader_segment_valid(segment)) return "bad segment";

		uint64_t page_start = segment->virtual_address & ~(LOADER_PAGE_SIZE - 1ULL);
		uint64_t page_end = (segment->virtual_address + segment->memory_size + LOADER_PAGE_SIZE - 1ULL) & ~(LOADER_PAGE_SIZE - 1ULL);

		for (uint32_t other = 0U; other < index; other++) {
			const loader_segment_t *earlier = &segments[other];

			if (earlier->type != ELF_PT_LOAD) continue;

			uint64_t earlier_start = earlier->virtual_address & ~(LOADER_PAGE_SIZE - 1ULL);
			uint64_t earlier_end = (earlier->virtual_address + earlier->memory_size + LOADER_PAGE_SIZE - 1ULL) & ~(LOADER_PAGE_SIZE - 1ULL);

			if (page_start < earlier_end && earlier_start < page_end) return "segments share a page";
		}

		if ((segment->flags & ELF_PF_X) != 0U && entry_page >= page_start && entry_page < page_end) entry_ok = true;

		info->segment_count++;
		if (page_start < info->image_low) info->image_low = page_start;
		if (page_end > info->image_high) info->image_high = page_end;
	}

	if (info->segment_count == 0U) return "no loadable segment";
	if (!entry_ok) return "entry not in executable segment";
	return 0;
}

#endif
