OUTPUT_ARCH(aarch64)
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
