/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        makedefs/linker-x86_64.ld
 *
 * Identity-linked ELF64 image loaded by the x86-64 UEFI loader.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);
	rodata PT_LOAD FLAGS(4);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = 0x00100000;
	__kernel_start = .;

	.text :
	{
		__text_start = .;
		KEEP(*(.text.boot))
		*(.text .text.*)
		. = ALIGN(4096);
		__text_end = .;
	} :text

	.rodata :
	{
		__rodata_start = .;
		*(.rodata .rodata.*)
		. = ALIGN(4096);
		__rodata_end = .;
	} :rodata

	.data :
	{
		__data_start = .;
		*(.data .data.*)
		. = ALIGN(4096);
		__data_end = .;
	} :data

	.bss :
	{
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(4096);
		__bss_end = .;
	} :data

	__kernel_end = .;

	/DISCARD/ :
	{
		*(.comment)
		*(.note .note.*)
	}
}
