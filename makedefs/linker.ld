ENTRY(_start)

KERNEL_PHYS_BASE = 0x40080000;
KERNEL_VIRT_OFFSET = 0xFFFFFF8000000000;
KERNEL_VIRT_BASE = KERNEL_VIRT_OFFSET + KERNEL_PHYS_BASE;

SECTIONS
{
	/*
	 * QEMU places the raw kernel image at KERNEL_PHYS_BASE. The ELF is
	 * linked at its permanent TTBR1 virtual address, while AT() keeps each
	 * section's load address in the physical image at the matching offset.
	 *
	 * During early boot the MMU is disabled and the CPU executes these same
	 * bytes through their physical alias. AArch64 PC-relative references keep
	 * working because the physical and virtual layouts differ by one constant.
	 */
	. = KERNEL_VIRT_BASE;

	__kernel_start = .;
	__kernel_phys_start = KERNEL_PHYS_BASE;

	.text : AT(ADDR(.text) - KERNEL_VIRT_OFFSET)
	{
		__text_start = .;

		KEEP(*(.text.boot))

		/* VBAR_EL1 requires the vector table to be 2048-byte aligned. */
		. = ALIGN(2048);
		KEEP(*(.text.vectors))

		*(.text .text.*)

		/* Never share a page between executable and non-executable data. */
		. = ALIGN(4096);
		__text_end = .;
	}

	__text_phys_start = LOADADDR(.text);
	__text_phys_end = LOADADDR(.text) + SIZEOF(.text);

	.rodata : AT(ADDR(.rodata) - KERNEL_VIRT_OFFSET)
	{
		__rodata_start = .;

		*(.rodata .rodata.*)
		*(.data.rel.ro .data.rel.ro.*)

		. = ALIGN(4096);
		__rodata_end = .;
	}

	__rodata_phys_start = LOADADDR(.rodata);
	__rodata_phys_end = LOADADDR(.rodata) + SIZEOF(.rodata);

	.data : AT(ADDR(.data) - KERNEL_VIRT_OFFSET)
	{
		__data_start = .;

		*(.data .data.*)
		*(.sdata .sdata.*)

		. = ALIGN(4096);
		__data_end = .;
	}

	__data_phys_start = LOADADDR(.data);
	__data_phys_end = LOADADDR(.data) + SIZEOF(.data);

	.bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VIRT_OFFSET)
	{
		__bss_start = .;

		*(.bss .bss.*)
		*(.sbss .sbss.*)
		*(COMMON)

		. = ALIGN(4096);
		__bss_end = .;
	}

	__bss_phys_start = LOADADDR(.bss);
	__bss_phys_end = LOADADDR(.bss) + SIZEOF(.bss);

	__kernel_end = .;
	__kernel_phys_end = __kernel_end - KERNEL_VIRT_OFFSET;

	ASSERT(__kernel_phys_start == KERNEL_PHYS_BASE,
		"kernel physical base changed unexpectedly")
	ASSERT(__kernel_start - __kernel_phys_start == KERNEL_VIRT_OFFSET,
		"kernel VMA/LMA offset mismatch")
	ASSERT(__kernel_end - __kernel_phys_end == KERNEL_VIRT_OFFSET,
		"kernel end VMA/LMA offset mismatch")

	/DISCARD/ :
	{
		*(.comment)
		*(.eh_frame .eh_frame.*)
		*(.note .note.*)
	}
}
