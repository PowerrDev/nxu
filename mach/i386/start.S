/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/start.S
 *
 * i386 kernel entry. A Multiboot loader (QEMU -kernel, GRUB) enters here in
 * 32-bit protected mode with paging off, %eax holding the Multiboot magic
 * and %ebx the physical address of the boot-information structure.
 */

#define MULTIBOOT_HEADER_MAGIC 0x1BADB002
/* Bit 0: align modules on pages. Bit 1: provide memory information. */
#define MULTIBOOT_HEADER_FLAGS 0x00000003

.section .multiboot, "a"
.balign 4

	.long MULTIBOOT_HEADER_MAGIC
	.long MULTIBOOT_HEADER_FLAGS
	.long -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

.section .text.boot, "ax"
.global _start
.type _start, @function

.extern __bss_start
.extern __bss_end

_start:
	cli
	cld

	/* Keep the Multiboot registers safe across the bss clear. */
	movl %eax, %esi
	movl %ebx, %edi

	movl $i386_boot_stack_top, %esp
	xorl %ebp, %ebp

	movl $__bss_start, %edx
	movl $__bss_end, %ecx
	subl %edx, %ecx
	shrl $2, %ecx
	xorl %eax, %eax
	xchgl %edx, %edi
	rep stosl
	movl %edx, %edi

	/* i386_init(magic, info): 8 bytes of padding keep %esp 16-aligned at the call. */
	subl $8, %esp
	pushl %edi
	pushl %esi
	call i386_init

.Lhalt:
	cli
	hlt
	jmp .Lhalt

.size _start, . - _start

.section .bss.stack, "aw", @nobits
.balign 16

/* Exported so the trap code can bound its backtrace to the boot stack. */
.global i386_boot_stack_bottom
.global i386_boot_stack_top

i386_boot_stack_bottom:
	.skip 16384

i386_boot_stack_top:
