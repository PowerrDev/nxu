/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        boot/uefi/arm64.c
 *
 * NBL's back end for AArch64 firmware. Loads the arm64 kernel's ELF64 image
 * at its physical addresses and enters it the way a direct boot does: MMU
 * off, the Device Tree address in x0.
 *
 * The Device Tree is the whole handoff. The kernel is given a copy of the
 * firmware's tree in which /chosen/bootargs is the boot volume's argument
 * line and the memory reservation block names the memory the boot chime is
 * still playing from.
 */

#include "fdt.h"
#include "nbl.h"

#define ELF_MACHINE_AARCH64 183U
#define ELF_TYPE_EXECUTABLE 2U
#define ELF_PROGRAM_LOAD 1U
#define ELF_PROGRAM_EXECUTE 1U

#define PAGE_SIZE 4096ULL

/* UEFI memory allocation: AllocateAnyPages, AllocateAddress; EfiLoaderData. */
#define ALLOCATE_ANY 0U
#define ALLOCATE_ADDRESS 2U
#define MEMORY_LOADER_DATA 2U

struct __attribute__((packed)) elf64_header {
	uint8_t ident[16];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint64_t entry;
	uint64_t phoff;
	uint64_t shoff;
	uint32_t flags;
	uint16_t ehsize;
	uint16_t phsize;
	uint16_t phcount;
	uint16_t shsize;
	uint16_t shcount;
	uint16_t shstr;
};

struct __attribute__((packed)) elf64_program {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t vaddr;
	uint64_t paddr;
	uint64_t filesz;
	uint64_t memsz;
	uint64_t align;
};

void nbl_arm64_clean_range(uint64_t start, uint64_t end);
void nbl_arm64_enter(uint64_t entry, uint64_t device_tree) __attribute__((noreturn));

/* What nbl_arch_load placed, for nbl_arch_enter. */
static uint64_t kernel_entry;
static uint64_t kernel_low;
static uint64_t kernel_high;

/*
 * Routine:     nbl_arch_load
 * Purpose:
 *              Load the ELF64 kernel. It is linked at its permanent virtual
 *              addresses and loaded at the physical ones its program headers
 *              name; the entry point is entered through the physical alias.
 */
status
nbl_arch_load(const uint8_t *data, uintn size)
{
	if (size < sizeof(struct elf64_header)) {
		return EFI_LOAD_ERROR;
	}

	const struct elf64_header *header = (const void *)data;

	if (!nbl_equal(header->ident, "\177ELF\2\1\1", 7) || header->type != ELF_TYPE_EXECUTABLE ||
		header->machine != ELF_MACHINE_AARCH64 || header->version != 1 ||
		header->phsize != sizeof(struct elf64_program) || header->phcount == 0 || header->phcount > 64) {
		nbl_log("nbl_arch_load: not an AArch64 ELF64 executable");
		return EFI_LOAD_ERROR;
	}

	if (header->phoff > size || (uint64_t)header->phcount * header->phsize > size - header->phoff) {
		return EFI_LOAD_ERROR;
	}

	uint64_t low = UINT64_MAX;
	uint64_t high = 0;
	bool entry_found = false;

	for (uint32_t index = 0; index < header->phcount; index++) {
		const struct elf64_program *program = (const void *)(data + header->phoff + index * header->phsize);

		if (program->type != ELF_PROGRAM_LOAD || program->memsz == 0) {
			continue;
		}

		if (program->filesz > program->memsz || program->offset > size || program->filesz > size - program->offset ||
			program->paddr > UINT64_MAX - program->memsz || program->vaddr > UINT64_MAX - program->memsz) {
			return EFI_LOAD_ERROR;
		}

		if ((program->flags & ELF_PROGRAM_EXECUTE) && header->entry >= program->vaddr &&
			header->entry - program->vaddr < program->memsz) {
			kernel_entry = header->entry - program->vaddr + program->paddr;
			entry_found = true;
		}

		if (program->paddr < low) {
			low = program->paddr;
		}

		if (program->paddr + program->memsz > high) {
			high = program->paddr + program->memsz;
		}
	}

	if (!entry_found || high <= low || high > UINT64_MAX - PAGE_SIZE) {
		return EFI_LOAD_ERROR;
	}

	low &= ~(PAGE_SIZE - 1);
	high = (high + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	uint64_t address = low;
	status result = bs->allocate_pages(ALLOCATE_ADDRESS, MEMORY_LOADER_DATA, (high - low) / PAGE_SIZE, &address);

	if (EFI_ERROR(result)) {
		nbl_log_hex("nbl_arch_load: the firmware holds memory the kernel is linked for, from ", low);
		return result;
	}

	nbl_zero((void *)(uintptr_t)low, high - low);
	for (uint32_t index = 0; index < header->phcount; index++) {
		const struct elf64_program *program = (const void *)(data + header->phoff + index * header->phsize);

		if (program->type == ELF_PROGRAM_LOAD) {
			nbl_copy((void *)(uintptr_t)program->paddr, data + program->offset, program->filesz);
		}
	}

	kernel_low = low;
	kernel_high = high;
	return 0;
}

/*
 * Routine:     nbl_arch_enter
 * Purpose:
 *              Write the kernel's Device Tree, exit boot services, and enter
 *              the kernel with the MMU off.
 */
status
nbl_arch_enter(handle image, const char *arguments)
{
	const void *firmware_tree = nbl_fdt_firmware();

	if (!firmware_tree) {
		nbl_log("nbl_arch_enter: the firmware publishes no Device Tree (boot the machine with ACPI off)");
		return EFI_NOT_FOUND;
	}

	/* Room for the copy: the tree, the argument line, one reservation, the new name and padding. */
	uint64_t capacity = (uint64_t)nbl_fdt_size(firmware_tree) + nbl_length(arguments) + 256U;
	uint64_t tree_pages = (capacity + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t tree = 0;
	status s = bs->allocate_pages(ALLOCATE_ANY, MEMORY_LOADER_DATA, tree_pages, &tree);

	if (EFI_ERROR(s)) {
		return s;
	}

	uint64_t chime_base = 0, chime_size = 0;

	nbl_chime_region(&chime_base, &chime_size);

	uint32_t tree_size = nbl_fdt_rebuild(firmware_tree, (void *)(uintptr_t)tree, (uint32_t)(tree_pages * PAGE_SIZE),
										 arguments, chime_base, chime_size);

	if (!tree_size) {
		nbl_log("nbl_arch_enter: the firmware's Device Tree cannot be rewritten");
		return EFI_LOAD_ERROR;
	}

	/* Allocate map storage before acquiring the final ExitBootServices key. */
	uintn map_size = 0, key = 0, descriptor_size = 0;
	uint32_t descriptor_version = 0;

	s = bs->get_memory_map(&map_size, 0, &key, &descriptor_size, &descriptor_version);
	if (s != EFI_BUFFER_TOO_SMALL || !descriptor_size) {
		return EFI_LOAD_ERROR;
	}

	map_size += descriptor_size * 32;

	void *map = 0;

	s = bs->allocate_pool(MEMORY_LOADER_DATA, map_size, &map);
	if (EFI_ERROR(s)) {
		return s;
	}

	uintn map_capacity = map_size;

	nbl_log("nbl_arch_enter: entering arm64 kernel");

	/* A stale map key requires a fresh map; do not allocate inside this loop. */
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		map_size = map_capacity;
		s = bs->get_memory_map(&map_size, map, &key, &descriptor_size, &descriptor_version);
		if (EFI_ERROR(s)) {
			return s;
		}

		s = bs->exit_boot_services(image, key);
		if (!EFI_ERROR(s)) {
			break;
		}

		if (s != EFI_INVALID_PARAMETER) {
			return s;
		}
	}

	if (EFI_ERROR(s)) {
		return s;
	}

	nbl_arm64_clean_range(kernel_low, kernel_high);
	nbl_arm64_clean_range(tree, tree + tree_size);
	nbl_arm64_enter(kernel_entry, tree);
}
