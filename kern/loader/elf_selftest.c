/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/loader/elf_selftest.c
 *
 * Boot self-test for the ELF32 executable checks (boot argument
 * "test=userland"). It needs neither the VFS nor the VM: it assembles small
 * ELF32/EM_386 images by hand in a static buffer and runs them through
 * loader_image_inspect(), which is built from the same decode and validation
 * helpers (kern/loader/elf_format.h) loader_spawn() uses on i386. One valid
 * image, then one deliberately broken image per validation the loader is
 * meant to apply: bounds, overflow, alignment, no W+X, the user address
 * window, file bounds, entry point placement.
 *
 * Launching bootd end to end is a later step (it needs vfs, vm and threads);
 * i386_init_userland is deliberately left at its weak default until then.
 */

#include <kern/i386/boot_info.h>

#include <kern/console/console.h>
#include <kern/loader/elf_format.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TEST_IMAGE_SIZE 0x3000U
#define TEST_TEXT_VADDR 0x00401000U
#define TEST_DATA_VADDR 0x00402000U
#define TEST_TEXT_OFFSET 0x1000U
#define TEST_DATA_OFFSET 0x2000U
#define TEST_PHOFF 52U
#define TEST_PHDR_SIZE 32U

#define PT_NOTE 4U
#define PF_R 0x4U

static uint8_t g_image[TEST_IMAGE_SIZE];

static uint32_t g_cases;
static uint32_t g_failures;

static void
put16(uint32_t offset, uint32_t value)
{
	g_image[offset] = (uint8_t)value;
	g_image[offset + 1U] = (uint8_t)(value >> 8U);
}

static void
put32(uint32_t offset, uint32_t value)
{
	put16(offset, value & 0xFFFFU);
	put16(offset + 2U, value >> 16U);
}

/* Field offsets inside one ELF32 program header. */
#define PH_TYPE 0U
#define PH_OFFSET 4U
#define PH_VADDR 8U
#define PH_FILESZ 16U
#define PH_MEMSZ 20U
#define PH_FLAGS 24U

static uint32_t
phdr(uint32_t index)
{
	return TEST_PHOFF + index * TEST_PHDR_SIZE;
}

static void
set_segment(uint32_t index, uint32_t type, uint32_t offset, uint32_t vaddr, uint32_t filesz, uint32_t memsz, uint32_t flags)
{
	uint32_t base = phdr(index);

	put32(base + PH_TYPE, type);
	put32(base + PH_OFFSET, offset);
	put32(base + PH_VADDR, vaddr);
	put32(base + 12U, vaddr);
	put32(base + PH_FILESZ, filesz);
	put32(base + PH_MEMSZ, memsz);
	put32(base + PH_FLAGS, flags);
	put32(base + 28U, 0x1000U);
}

/* A well-formed two-segment image, shaped like what makedefs/user-i386.ld links. */
static void
build_valid(void)
{
	memset(g_image, 0, sizeof(g_image));

	g_image[0] = 0x7FU;
	g_image[1] = 'E';
	g_image[2] = 'L';
	g_image[3] = 'F';
	g_image[4] = ELF_CLASS_32;
	g_image[5] = ELF_DATA_LSB;
	g_image[6] = ELF_VERSION_CURRENT;

	put16(16U, ELF_TYPE_EXEC);
	put16(18U, ELF_MACHINE_386);
	put32(20U, ELF_VERSION_CURRENT);
	put32(24U, TEST_TEXT_VADDR);
	put32(28U, TEST_PHOFF);
	put16(40U, sizeof(elf32_header_t));
	put16(42U, TEST_PHDR_SIZE);
	put16(44U, 2U);

	set_segment(0U, ELF_PT_LOAD, TEST_TEXT_OFFSET, TEST_TEXT_VADDR, 0x100U, 0x100U, PF_R | ELF_PF_X);
	set_segment(1U, ELF_PT_LOAD, TEST_DATA_OFFSET, TEST_DATA_VADDR, 0x80U, 0x1000U, PF_R | ELF_PF_W);
}

static void
check(const char *name, uint32_t size, const char *expected)
{
	loader_image_info_t info;
	const char *reason = loader_image_inspect(g_image, size, &info);
	bool pass = expected == 0 ? reason == 0 : (reason != 0 && strcmp(reason, expected) == 0);

	g_cases++;

	if (pass) return;

	g_failures++;
	kprintf("i386_init_userland_selftest: FAIL %s: expected %s, got %s\n", name, expected != 0 ? expected : "accept", reason != 0 ? reason : "accept");
}

static void
check_valid_image(void)
{
	loader_image_info_t info;

	build_valid();
	check("valid image", TEST_IMAGE_SIZE, 0);

	const char *reason = loader_image_inspect(g_image, TEST_IMAGE_SIZE, &info);

	g_cases++;

	/* The widened fields must land where the ELF32 layout puts them (flags follow memsz). */
	if (reason != 0 || info.entry != TEST_TEXT_VADDR || info.segment_count != 2U || info.image_low != 0x00401000ULL || info.image_high != 0x00403000ULL) {
		g_failures++;
		kprintf(
			"i386_init_userland_selftest: FAIL decoded fields: entry 0x%x, %u segments, [0x%x, 0x%x)\n",
			(uint32_t)info.entry,
			info.segment_count,
			(uint32_t)info.image_low,
			(uint32_t)info.image_high
		);
	}
}

static void
check_header_rejections(void)
{
	build_valid();
	g_image[4] = ELF_CLASS_64;
	check("ELF64 class", TEST_IMAGE_SIZE, "bad header");

	build_valid();
	put16(18U, ELF_MACHINE_AARCH64);
	check("EM_AARCH64 machine", TEST_IMAGE_SIZE, "bad header");

	build_valid();
	put16(16U, 3U);
	check("ET_DYN type", TEST_IMAGE_SIZE, "bad header");

	build_valid();
	g_image[5] = 2U;
	check("big-endian data", TEST_IMAGE_SIZE, "bad header");

	build_valid();
	g_image[0] = 0x7EU;
	check("bad magic", TEST_IMAGE_SIZE, "bad header");

	build_valid();
	put32(24U, 0U);
	check("zero entry", TEST_IMAGE_SIZE, "bad header");

	build_valid();
	put16(44U, 0U);
	check("no program headers", TEST_IMAGE_SIZE, "bad header");

	build_valid();
	put16(44U, ELF_PROGRAM_HEADER_MAX + 1U);
	check("too many program headers", TEST_IMAGE_SIZE, "bad header");

	build_valid();
	put16(42U, sizeof(elf64_program_header_t));
	check("wrong program header size", TEST_IMAGE_SIZE, "bad header");

	build_valid();
	put16(40U, sizeof(elf64_header_t));
	check("wrong header size", TEST_IMAGE_SIZE, "bad header");

	build_valid();
	check("truncated header", sizeof(elf32_header_t) - 1U, "truncated header");

	build_valid();
	put32(28U, TEST_IMAGE_SIZE - 16U);
	check("program table past end of file", TEST_IMAGE_SIZE, "program table outside file");

	build_valid();
	put32(28U, 0xFFFFFFF0U);
	check("program table offset wraps", TEST_IMAGE_SIZE, "program table outside file");
}

static void
check_segment_rejections(void)
{
	build_valid();
	set_segment(1U, ELF_PT_LOAD, TEST_DATA_OFFSET, TEST_DATA_VADDR, 0x80U, 0x1000U, PF_R | ELF_PF_W | ELF_PF_X);
	check("W+X segment", TEST_IMAGE_SIZE, "bad segment");

	build_valid();
	set_segment(0U, ELF_PT_LOAD, TEST_TEXT_OFFSET, 0x00000000U, 0x100U, 0x100U, PF_R | ELF_PF_X);
	put32(24U, 0x00001000U);
	check("segment at the null page", TEST_IMAGE_SIZE, "bad segment");

	build_valid();
	set_segment(0U, ELF_PT_LOAD, TEST_TEXT_OFFSET, TEST_TEXT_VADDR + 0x10U, 0x100U, 0x100U, PF_R | ELF_PF_X);
	check("vaddr/offset misaligned", TEST_IMAGE_SIZE, "bad segment");

	build_valid();
	set_segment(1U, ELF_PT_LOAD, TEST_DATA_OFFSET, (uint32_t)LOADER_USER_STACK_BASE - 0x1000U, 0x80U, 0x2000U, PF_R | ELF_PF_W);
	check("segment reaches the stack", TEST_IMAGE_SIZE, "bad segment");

	build_valid();
	set_segment(1U, ELF_PT_LOAD, TEST_DATA_OFFSET, 0xC0000000U, 0x80U, 0x1000U, PF_R | ELF_PF_W);
	check("segment in kernel space", TEST_IMAGE_SIZE, "bad segment");

	build_valid();
	set_segment(1U, ELF_PT_LOAD, TEST_DATA_OFFSET, 0xFFFFF000U, 0x80U, 0x2000U, PF_R | ELF_PF_W);
	check("segment wraps the address space", TEST_IMAGE_SIZE, "bad segment");

	build_valid();
	set_segment(1U, ELF_PT_LOAD, TEST_DATA_OFFSET, TEST_DATA_VADDR, 0x1000U, 0x80U, PF_R | ELF_PF_W);
	check("memsz smaller than filesz", TEST_IMAGE_SIZE, "bad segment");

	build_valid();
	set_segment(1U, ELF_PT_LOAD, TEST_DATA_OFFSET, TEST_DATA_VADDR, 0U, 0U, PF_R | ELF_PF_W);
	check("empty segment", TEST_IMAGE_SIZE, "bad segment");

	build_valid();
	set_segment(1U, ELF_PT_LOAD, TEST_DATA_OFFSET, TEST_DATA_VADDR, 0x10000U, 0x10000U, PF_R | ELF_PF_W);
	check("file bytes past end of file", TEST_IMAGE_SIZE, "segment outside file");

	build_valid();
	set_segment(1U, ELF_PT_LOAD, 0xFFFFF000U | (TEST_DATA_VADDR & 0xFFFU), TEST_DATA_VADDR, 0x2000U, 0x2000U, PF_R | ELF_PF_W);
	check("file offset + size wraps 32 bits", TEST_IMAGE_SIZE, "segment outside file");

	build_valid();
	set_segment(1U, ELF_PT_LOAD, TEST_DATA_OFFSET + 0x80U, TEST_TEXT_VADDR + 0x80U, 0x80U, 0x80U, PF_R | ELF_PF_W);
	check("two segments share a page", TEST_IMAGE_SIZE, "segments share a page");

	build_valid();
	put32(24U, TEST_DATA_VADDR);
	check("entry in a data segment", TEST_IMAGE_SIZE, "entry not in executable segment");

	build_valid();
	put32(24U, 0x00500000U);
	check("entry outside every segment", TEST_IMAGE_SIZE, "entry not in executable segment");
}

static void
check_segment_kinds(void)
{
	/* Non-PT_LOAD entries are skipped whatever they claim. */
	build_valid();
	put16(44U, 3U);
	set_segment(2U, PT_NOTE, 0xFFFFFFF0U, 0U, 0xFFFFU, 0U, ELF_PF_W | ELF_PF_X);
	check("PT_NOTE ignored", TEST_IMAGE_SIZE, 0);

	build_valid();
	set_segment(0U, PT_NOTE, TEST_TEXT_OFFSET, TEST_TEXT_VADDR, 0x100U, 0x100U, PF_R | ELF_PF_X);
	set_segment(1U, PT_NOTE, TEST_DATA_OFFSET, TEST_DATA_VADDR, 0x80U, 0x1000U, PF_R | ELF_PF_W);
	check("no PT_LOAD at all", TEST_IMAGE_SIZE, "no loadable segment");

	/* A read-only segment on its own page is fine, a bss-only data segment too. */
	build_valid();
	put16(44U, 3U);
	set_segment(2U, ELF_PT_LOAD, 0x0000U, 0x00403000U, 0U, 0x4000U, PF_R | ELF_PF_W);
	check("bss-only segment", TEST_IMAGE_SIZE, 0);
}

bool
i386_init_userland_selftest(const i386_boot_info_t *boot)
{
	(void)boot;

	g_cases = 0U;
	g_failures = 0U;

	check_valid_image();
	check_header_rejections();
	check_segment_rejections();
	check_segment_kinds();

	kprintf("i386_init_userland_selftest: %u ELF32 image cases, %u failed\n", g_cases, g_failures);
	return g_failures == 0U;
}
