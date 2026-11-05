/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        boot/uefi/main.c
 *
 * Loads either the i386 Multiboot kernel or the native x86-64 kernel and
 * passes its architecture-specific handoff after ExitBootServices.
 */

#include "efi.h"
#include <boot/handoff.h>

#include <stdbool.h>

static guid loaded_guid = {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static guid fs_guid = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static guid file_info_guid = {0x9576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static guid gop_guid = {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
static guid acpi2_guid = {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
static guid acpi1_guid = {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
static boot_services *bs;
static system_table *st;

/*
 * Routine:     copy
 * Purpose:
 *              Copy a bounded byte range without relying on a UEFI C runtime.
 */
static void
copy(void *d, const void *s, uintn n)
{
	uint8_t *a = d;
	const uint8_t *b = s;

	while (n--) {
		*a++ = *b++;
	}
}

/*
 * Routine:     zero
 * Purpose:
 *              Clear a memory range used for a kernel image or boot handoff.
 */
static void
zero(void *d, uintn n)
{
	uint8_t *a = d;

	while (n--) {
		*a++ = 0;
	}
}

/*
 * Routine:     equal
 * Purpose:
 *              Compare fixed-size firmware identifiers without libc support.
 */
static int
equal(const void *a, const void *b, uintn n)
{
	const uint8_t *x = a, *y = b;

	while (n--) {
		if (*x++ != *y++) {
			return 0;
		}
	}

	return 1;
}

/*
 * Routine:     log_line
 * Purpose:
 *              Send a UTF-16 status line to the UEFI console before handoff.
 */
static void
log_line(char16 *message)
{
	st->console_out->output(st->console_out, message);
}

/*
 * Routine:     read_file
 * Purpose:
 *              Read one bounded file from the EFI system partition using the
 *              filesystem protocol attached to this loader image.
 */
static status
read_file(handle image, char16 *path, void **out, uintn *size)
{
	loaded_image *loaded = 0;
	filesystem *fs = 0;
	file *root = 0, *f = 0;
	status s = bs->handle_protocol(image, &loaded_guid, (void **)&loaded);

	if (EFI_ERROR(s)) {
		return s;
	}

	s = bs->handle_protocol(loaded->device, &fs_guid, (void **)&fs);
	if (EFI_ERROR(s)) {
		return s;
	}

	s = fs->open_volume(fs, &root);
	if (EFI_ERROR(s)) {
		return s;
	}

	s = root->open(root, &f, path, 1, 0);
	root->close(root);
	if (EFI_ERROR(s)) {
		return s;
	}

	uint8_t info[1024];
	uintn info_size = sizeof(info);

	s = f->get_info(f, &file_info_guid, &info_size, info);
	if (EFI_ERROR(s)) {
		f->close(f);
		return s;
	}

	uint64_t length;

	copy(&length, info + 8, 8);
	if (!length || length > 64 * 1024 * 1024) {
		f->close(f);
		return EFI_LOAD_ERROR;
	}

	*size = length;
	s = bs->allocate_pool(2, *size, out);
	if (!EFI_ERROR(s)) {
		uintn read_size = *size;

		s = f->read(f, &read_size, *out);
		if (!EFI_ERROR(s) && read_size != *size) {
			s = EFI_LOAD_ERROR;
		}
	}

	f->close(f);
	return s;
}

struct __attribute__((packed)) elf_header {
	uint8_t ident[16];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint32_t entry;
	uint32_t phoff;
	uint32_t shoff;
	uint32_t flags;
	uint16_t ehsize;
	uint16_t phsize;
	uint16_t phcount;
	uint16_t shsize;
	uint16_t shcount;
	uint16_t shstr;
};

struct elf_program {
	uint32_t type;
	uint32_t offset;
	uint32_t vaddr;
	uint32_t paddr;
	uint32_t filesz;
	uint32_t memsz;
	uint32_t flags;
	uint32_t align;
};

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

struct __attribute__((packed)) nxu_boot_i386_memory_map_entry {
	uint32_t size;
	uint64_t address;
	uint64_t length;
	uint32_t type;
};

_Static_assert(sizeof(struct nxu_boot_i386_memory_map_entry) == 24U, "i386 Multiboot memory-map entry layout");

/*
 * Routine:     load_kernel
 * Purpose:
 *              Load the existing ELF32 kernel at its physical addresses for
 *              the established long-mode-to-i386 transition.
 */
static status
load_kernel(const uint8_t *data, uintn size, uint32_t *entry)
{
	if (size < sizeof(struct elf_header)) {
		return EFI_LOAD_ERROR;
	}

	const struct elf_header *h = (const void *)data;

	if (!equal(h->ident, "\177ELF\1\1\1", 7) || h->type != 2 || h->machine != 3 || h->version != 1 ||
		h->phsize != sizeof(struct elf_program) || !h->phcount || h->phcount > 64) {
		return EFI_LOAD_ERROR;
	}

	if (h->phoff > size || (uintn)h->phcount * h->phsize > size - h->phoff) {
		return EFI_LOAD_ERROR;
	}

	uint32_t low = 0xffffffff, high = 0;
	int executable_entry = 0;

	for (unsigned i = 0; i < h->phcount; i++) {
		const struct elf_program *p = (const void *)(data + h->phoff + i * h->phsize);

		if (p->type != 1) {
			continue;
		}

		if (p->filesz > p->memsz || p->offset > size || p->filesz > size - p->offset || p->paddr < 0x100000 ||
			p->paddr >= 0x28000000 || p->memsz > 0x28000000 - p->paddr) {
			return EFI_LOAD_ERROR;
		}

		if ((p->flags & 1) && h->entry >= p->paddr && h->entry - p->paddr < p->memsz) {
			executable_entry = 1;
		}

		if (p->paddr < low) {
			low = p->paddr;
		}

		if (p->paddr + p->memsz > high) {
			high = p->paddr + p->memsz;
		}
	}

	if (!executable_entry || high <= low) {
		return EFI_LOAD_ERROR;
	}

	low &= ~4095U;
	high = (high + 4095U) & ~4095U;

	uint64_t address = low;
	status s = bs->allocate_pages(2, 2, (high - low) / 4096, &address);

	if (EFI_ERROR(s)) {
		return s;
	}

	zero((void *)(uintptr_t)low, high - low);
	for (unsigned i = 0; i < h->phcount; i++) {
		const struct elf_program *p = (const void *)(data + h->phoff + i * h->phsize);

		if (p->type == 1) {
			copy((void *)(uintptr_t)p->paddr, data + p->offset, p->filesz);
		}
	}

	*entry = h->entry;
	return 0;
}

/*
 * Routine:     load_kernel64
 * Purpose:
 *              Load a low-memory ELF64 kernel that runs directly in the
 *              firmware's current long mode after boot services exit.
 */
static status
load_kernel64(const uint8_t *data, uintn size, uint64_t *entry)
{
	if (size < sizeof(struct elf64_header)) {
		return EFI_LOAD_ERROR;
	}

	const struct elf64_header *header = (const void *)data;
	if (!equal(header->ident, "\177ELF\2\1\1", 7) || header->type != 2 || header->machine != 62 ||
		header->version != 1 || header->phsize != sizeof(struct elf64_program) || header->phcount == 0 ||
		header->phcount > 64) {
		return EFI_LOAD_ERROR;
	}

	if (header->phoff > size || (uint64_t)header->phcount * header->phsize > size - header->phoff) {
		return EFI_LOAD_ERROR;
	}

	uint64_t low = UINT64_MAX;
	uint64_t high = 0;
	bool executable_entry = false;

	for (uint32_t index = 0; index < header->phcount; index++) {
		const struct elf64_program *program = (const void *)(data + header->phoff + index * header->phsize);
		if (program->type != 1) {
			continue;
		}

		if (program->filesz > program->memsz || program->offset > size || program->filesz > size - program->offset ||
			program->paddr < 0x100000 || program->paddr >= 0x40000000 || program->memsz > 0x40000000 - program->paddr) {
			return EFI_LOAD_ERROR;
		}

		if ((program->flags & 1U) != 0U && header->entry >= program->paddr &&
			header->entry - program->paddr < program->memsz) {
			executable_entry = true;
		}

		if (program->paddr < low) {
			low = program->paddr;
		}

		if (program->paddr + program->memsz > high) {
			high = program->paddr + program->memsz;
		}
	}

	if (!executable_entry || high <= low) {
		return EFI_LOAD_ERROR;
	}

	low &= ~4095ULL;
	if (high > UINT64_MAX - 4095ULL) {
		return EFI_LOAD_ERROR;
	}

	high = (high + 4095ULL) & ~4095ULL;

	uint64_t address = low;
	status result = bs->allocate_pages(2, 2, (high - low) / 4096, &address);
	if (EFI_ERROR(result)) {
		return result;
	}

	zero((void *)(uintptr_t)low, high - low);
	for (uint32_t index = 0; index < header->phcount; index++) {
		const struct elf64_program *program = (const void *)(data + header->phoff + index * header->phsize);

		if (program->type != 1) {
			continue;
		}

		copy((void *)(uintptr_t)program->paddr, data + program->offset, program->filesz);
	}

	*entry = header->entry;
	return 0;
}

extern uint8_t nxu_transition_start[], nxu_transition_end[], nxu_transition_entry[], nxu_transition_info[],
	nxu_transition_stack[];

/*
 * Routine:     efi_main
 * Purpose:
 *              Load the selected NXU ELF image, collect firmware handoff data,
 *              exit boot services, and transfer control to its entry point.
 */
status
efi_main(handle image, system_table *system)
{
	st = system;
	bs = system->boot;
	(void)bs->set_watchdog_timer(0, 0, 0, 0);
	log_line((char16 *)u"efi_main: preparing kernel handoff\r\n");

	void *kernel = 0;
	uintn kernel_size = 0;
	status s = read_file(image, (char16 *)u"\\NXU\\kernel.elf", &kernel, &kernel_size);

	if (EFI_ERROR(s)) {
		log_line((char16 *)u"efi_main: cannot read kernel image\r\n");
		return s;
	}

	bool kernel64 = kernel_size > 4 && ((const uint8_t *)kernel)[4] == 2;
	uint32_t entry32 = 0;
	uint64_t entry64 = 0;

	if (kernel64) {
		s = load_kernel64(kernel, kernel_size, &entry64);
	} else {
		s = load_kernel(kernel, kernel_size, &entry32);
		entry64 = entry32;
	}

	bs->free_pool(kernel);
	if (EFI_ERROR(s)) {
		log_line((char16 *)u"efi_main: kernel image is invalid or cannot be loaded\r\n");
		return s;
	}

	uint64_t handoff = 0x27ffffff;

	/* Keep the Multiboot handoff and transition code below the direct-map limit. */
	s = bs->allocate_pages(1, 1, 32, &handoff);
	if (EFI_ERROR(s)) {
		return s;
	}

	uint8_t *info = (void *)(uintptr_t)handoff;

	zero(info, 32 * 4096);

	uint32_t *fields = (void *)info;

	fields[0] = (1U << 6) | NXU_BOOT_FIRMWARE_FLAG;

	struct nxu_boot_firmware *fw = (void *)(info + NXU_BOOT_FIRMWARE_OFFSET);

	*fw = (struct nxu_boot_firmware){NXU_BOOT_FIRMWARE_MAGIC, 1, sizeof(*fw), 0};

	/* Prefer the ACPI 2.0 configuration table, falling back to ACPI 1.0. */
	for (uintn i = 0; i < st->table_count; i++) {
		if (equal(&st->tables[i].vendor, &acpi2_guid, sizeof(guid))) {
			fw->rsdp = (uint64_t)(uintptr_t)st->tables[i].table;
		}
	}

	if (!fw->rsdp) {
		for (uintn i = 0; i < st->table_count; i++) {
			if (equal(&st->tables[i].vendor, &acpi1_guid, sizeof(guid))) {
				fw->rsdp = (uint64_t)(uintptr_t)st->tables[i].table;
			}
		}
	}

	/* Describe the GOP framebuffer using the Multiboot video fields. */
	gop *graphics = 0;

	if (!EFI_ERROR(bs->locate_protocol(&gop_guid, 0, (void **)&graphics)) && graphics->mode && graphics->mode->info &&
		graphics->mode->info->format < 2) {
		gop_mode *m = graphics->mode;

		fields[0] |= 1U << 12;
		copy(info + 88, &m->framebuffer, 8);
		fields[24] = m->info->pixels_per_line * 4;
		fields[25] = m->info->width;
		fields[26] = m->info->height;
		info[108] = 32;
		info[109] = 1;
		info[110] = m->info->format == 0 ? 0 : 16;
		info[111] = 8;
		info[112] = 8;
		info[113] = 8;
		info[114] = m->info->format == 0 ? 16 : 0;
		info[115] = 8;
		log_line((char16 *)u"efi_main: GOP framebuffer discovered\r\n");
	}

	/* Copy optional boot arguments into the handoff allocation. */
	void *args = 0;
	uintn args_size = 0;
	uint64_t bootargs = 0;
	uint8_t *args_target = kernel64 ? info + 512 : info + 256;

	if (!EFI_ERROR(read_file(image, (char16 *)u"\\NXU\\bootargs.txt", &args, &args_size))) {
		if (args_size > 2047) {
			args_size = 2047;
		}

		copy(args_target, args, args_size);
		for (uintn i = 0; i < args_size; i++) {
			if (args_target[i] == '\n' || args_target[i] == '\r') {
				args_target[i] = ' ';
			}
		}

		args_target[args_size] = 0;
		bootargs = handoff + (kernel64 ? 512 : 256);

		if (!kernel64) {
			fields[4] = (uint32_t)handoff + 256;
			fields[0] |= 1U << 2;
		}

		bs->free_pool(args);
	}

	uint8_t *trampoline = info + 4096;

	if (!kernel64) {
		/* Patch the copied long-mode-to-i386 transition with this boot's addresses. */
		uintn trampoline_size = nxu_transition_end - nxu_transition_start;

		if (trampoline_size > 4096) {
			return EFI_LOAD_ERROR;
		}

		copy(trampoline, nxu_transition_start, trampoline_size);
		*(uint32_t *)(trampoline + (nxu_transition_entry - nxu_transition_start)) = entry32;
		*(uint32_t *)(trampoline + (nxu_transition_info - nxu_transition_start)) = (uint32_t)handoff;
		*(uint32_t *)(trampoline + (nxu_transition_stack - nxu_transition_start)) =
			(uint32_t)handoff + 32 * 4096 - 16;
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

	s = bs->allocate_pool(2, map_size, &map);
	if (EFI_ERROR(s)) {
		return s;
	}

	uintn capacity = map_size;

	log_line(kernel64 ? (char16 *)u"efi_main: entering x86-64 kernel\r\n" :
		(char16 *)u"efi_main: entering i386 kernel\r\n");

	/* A stale map key requires a fresh map; do not allocate inside this loop. */
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		map_size = capacity;
		s = bs->get_memory_map(&map_size, map, &key, &descriptor_size, &descriptor_version);
		if (EFI_ERROR(s)) {
			return s;
		}

		if (descriptor_size < sizeof(memory_descriptor) || map_size % descriptor_size) {
			return EFI_LOAD_ERROR;
		}

		uintn count = map_size / descriptor_size;

		uintn converted_entry_size = kernel64 ? sizeof(struct nxu_boot_memory_map_entry)
														 : sizeof(struct nxu_boot_i386_memory_map_entry);
		if (count * converted_entry_size > 24 * 4096) {
			return EFI_LOAD_ERROR;
		}

		if (kernel64) {
			struct nxu_boot_memory_map_entry *target = (void *)(info + 8192);

			for (uintn i = 0; i < count; i++) {
				memory_descriptor *d = (void *)((uint8_t *)map + i * descriptor_size);
				uint32_t type = d->type == 7 || d->type == 3 || d->type == 4 ? 1 : d->type == 9 ? 3 : d->type == 10 ? 4 : 2;

				target[i] = (struct nxu_boot_memory_map_entry){.size = sizeof(*target),
																	 .address = d->physical,
																	 .length = d->pages * 4096,
																	 .type = type};
			}
		} else {
			struct nxu_boot_i386_memory_map_entry *target = (void *)(info + 8192);

			for (uintn i = 0; i < count; i++) {
				memory_descriptor *d = (void *)((uint8_t *)map + i * descriptor_size);
				uint32_t type = d->type == 7 || d->type == 3 || d->type == 4 ? 1 : d->type == 9 ? 3 : d->type == 10 ? 4 : 2;

				target[i] = (struct nxu_boot_i386_memory_map_entry){20, d->physical, d->pages * 4096, type};
			}
		}

		fields[11] = count * converted_entry_size;
		fields[12] = (uint32_t)handoff + 8192;
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

	if (kernel64) {
		struct nxu_boot_x86_64 *xboot = (void *)(info + 256);

		*xboot = (struct nxu_boot_x86_64){
			NXU_BOOT_X86_64_MAGIC,
			NXU_BOOT_X86_64_VERSION,
			sizeof(*xboot),
			0,
			fw->rsdp,
			(uint64_t)(uintptr_t)(info + 8192),
			fields[11],
			sizeof(struct nxu_boot_memory_map_entry),
			bootargs,
			handoff + 32 * 4096 - 16,
		};

		typedef void (*x86_entry_t)(const struct nxu_boot_x86_64 *, uint64_t) __attribute__((sysv_abi));

		((x86_entry_t)(uintptr_t)entry64)(xboot, xboot->stack_top);
		__builtin_unreachable();
	}

	((void (*)(void))(uintptr_t)trampoline)();
	__builtin_unreachable();
}
