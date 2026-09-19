ENTRY(_start)

/*
 * i386 kernel image, linked and loaded at 1 MiB. There is no higher-half
 * alias yet: the Multiboot loader leaves paging off, so virtual and physical
 * addresses are the same until the x86 VM layer exists.
 */
KERNEL_BASE = 0x00100000;

SECTIONS
{
	. = KERNEL_BASE;

	__kernel_start = .;

	.text :
	{
		__text_start = .;

		/* The Multiboot header must sit within the first 8 KiB of the image. */
		KEEP(*(.multiboot))
		KEEP(*(.text.boot))

		*(.text .text.*)

		. = ALIGN(4096);
		__text_end = .;
	}

	.rodata :
	{
		__rodata_start = .;

		*(.rodata .rodata.*)

		. = ALIGN(4096);
		__rodata_end = .;
	}

	.data :
	{
		__data_start = .;

		*(.data .data.*)

		. = ALIGN(4096);
		__data_end = .;
	}

	.bss :
	{
		__bss_start = .;

		*(.bss .bss.*)
		*(COMMON)

		. = ALIGN(4096);
		__bss_end = .;
	}

	__kernel_end = .;

	/DISCARD/ :
	{
		*(.comment)
		*(.eh_frame .eh_frame.*)
		*(.note .note.*)
	}
}
