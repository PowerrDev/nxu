/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/gdt_load.S
 *
 * Loads the kernel GDT, reloads the code and data selectors, and loads TR.
 */

.section .text, "ax"
.global x86_gdt_load
.type x86_gdt_load, @function

/*
 * Routine:     x86_gdt_load
 * Purpose:
 *              Activate the supplied GDT and load the kernel's 64-bit code,
 *              data, and TSS selectors before IDT gates are installed.
 */
x86_gdt_load:
	lgdt (%rdi)
	pushq $0x08
	leaq .Lx86_gdt_reloaded(%rip), %rax
	pushq %rax
	lretq
.Lx86_gdt_reloaded:
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss
	xorw %ax, %ax
	movw %ax, %fs
	movw %ax, %gs
	movw $0x28, %ax
	ltr %ax
	retq

.size x86_gdt_load, . - x86_gdt_load
