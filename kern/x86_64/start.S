/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/start.S
 *
 * Native x86-64 kernel entry used by NxuBoot after ExitBootServices.
 */

.section .text.boot, "ax"
.global _start
.type _start, @function
.extern x86_kernel_main

/*
 * Routine:     _start
 * Purpose:
 *              Accept the NXU handoff in RDI and a kernel-owned stack top in
 *              RSI, then enter the first x86-64 C routine with SysV alignment.
 */
_start:
	cli
	cld
	movq %rsi, %rsp
	andq $-16, %rsp
	xorq %rbp, %rbp
	call x86_kernel_main

.Lhalt:
	hlt
	jmp .Lhalt

.size _start, . - _start

.global x86_enter_higher_half
.type x86_enter_higher_half, @function

/*
 * Routine:     x86_enter_higher_half
 * Purpose:
 *              Switch to the direct-map stack alias and jump into the same
 *              PC-relative kernel image through its higher-half alias.
 */
x86_enter_higher_half:
	cli
	movq %rdi, %r8
	movq %rsi, %rdi
	movq %rdx, %rsp
	andq $-16, %rsp
	subq $8, %rsp
	movq $0, (%rsp)
	jmp *%r8

.size x86_enter_higher_half, . - x86_enter_higher_half
