ENTRY(_start)

/*
 * i386 kernel image: loaded at physical 1 MiB, linked in the higher half.
 *
 * A Multiboot loader (QEMU -kernel, GRUB) copies each segment to its p_paddr
 * and jumps to e_entry untranslated, with paging off. So the boot stub lives
 * in its own section whose virtual address equals its load address, while
 * everything else is linked at 0xC0000000 + physical and loaded with AT():
 *
 *   physical 0x00100000  .boot   (VMA == LMA, runs with paging off)
 *   physical 0x00101000  .text   (VMA 0xC0101000)
 *   ...                  .rodata, .data, .bss, each page aligned
 *
 * The whole image is therefore one contiguous range in both address forms:
 * physical [0x100000, __kernel_phys_end) and virtual [__kernel_start,
 * __kernel_end) = physical + KERNEL_VIRT_OFFSET. The boot stub builds the
 * page tables that make the second form valid; see mach/i386/start.S.
 *
 * Sections are page aligned so the VM layer can give text and rodata
 * read-only mappings and everything else read-write.
 */
KERNEL_PHYS_BASE = 0x00100000;
KERNEL_VIRT_OFFSET = 0xC0000000;

PHDRS
{
	boot PT_LOAD FLAGS(5);
	text PT_LOAD FLAGS(5);
	rodata PT_LOAD FLAGS(4);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = KERNEL_PHYS_BASE;

	/* The Multiboot header must sit within the first 8 KiB of the image. */
	.boot :
	{
		KEEP(*(.multiboot))
		KEEP(*(.text.boot))
		KEEP(*(.data.boot))

		. = ALIGN(4096);
	} :boot

	/* Virtual address of the first byte of the image (the .boot page). */
	__kernel_start = KERNEL_PHYS_BASE + KERNEL_VIRT_OFFSET;
	__kernel_phys_start = KERNEL_PHYS_BASE;

	. += KERNEL_VIRT_OFFSET;

	.text : AT(ADDR(.text) - KERNEL_VIRT_OFFSET)
	{
		__text_start = .;

		*(.text .text.*)

		. = ALIGN(4096);
		__text_end = .;
	} :text

	.rodata : AT(ADDR(.rodata) - KERNEL_VIRT_OFFSET)
	{
		__rodata_start = .;

		*(.rodata .rodata.*)

		. = ALIGN(4096);
		__rodata_end = .;
	} :rodata

	.data : AT(ADDR(.data) - KERNEL_VIRT_OFFSET)
	{
		__data_start = .;

		*(.data .data.*)

		. = ALIGN(4096);
		__data_end = .;
	} :data

	.bss : AT(ADDR(.bss) - KERNEL_VIRT_OFFSET)
	{
		__bss_start = .;

		*(.bss .bss.*)
		*(COMMON)

		. = ALIGN(4096);
		__bss_end = .;
	} :data

	__kernel_end = .;
	__kernel_phys_end = __kernel_end - KERNEL_VIRT_OFFSET;

	/DISCARD/ :
	{
		*(.comment)
		*(.eh_frame .eh_frame.*)
		*(.note .note.*)
	}
}
