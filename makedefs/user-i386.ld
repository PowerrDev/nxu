/*
 * Linker script for i386 user programs. Same shape as makedefs/user.ld
 * (arm64): one read+execute segment holding text and read-only data, one
 * read+write segment holding data and bss, each starting on its own page so
 * the loader can give them different permissions with no page shared.
 *
 * The image is based at 0x00400000. The user window below the VM_MAP window
 * (kern/i386/vm_param.h) is [0x1000, 0x40000000); a program has to stay
 * inside it, and tools/check_i386_user.sh verifies that it does.
 *
 * No ELF header or program header table is placed in a segment, so the first
 * segment's file offset (page aligned by the linker) and its virtual address
 * are congruent modulo the page size, which is the only alignment rule the
 * loader enforces.
 */
OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = 0x400000;

	.text : ALIGN(4096)
	{
		KEEP(*(.text.start))
		*(.text .text.*)
		*(.rodata .rodata.*)
	} :text

	. = ALIGN(4096);

	.data : ALIGN(4096)
	{
		*(.data .data.*)
	} :data

	.bss (NOLOAD) : ALIGN(16)
	{
		*(.bss .bss.* COMMON)
	} :data

	/DISCARD/ :
	{
		*(.comment)
		*(.eh_frame*)
		*(.note*)
	}
}
