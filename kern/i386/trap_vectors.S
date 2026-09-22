/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/trap_vectors.S
 *
 * Per-vector entry stubs and the common trap path. Each stub normalises the
 * stack so every vector looks the same to the common code -- a dummy error
 * code where the CPU pushes none -- then jumps to i386_trap_common, which
 * finishes building an x86_saved_state32_t (see trap.h for the layout).
 */

#define KERNEL_DATA_SEL 0x10
#define KERNEL_PERCPU_SEL 0x40

/* Vector with no hardware error code: push a dummy 0, then the vector. */
.macro ISR_NOERR vector
	.text
	.p2align 4
i386_isr_\vector:
	pushl $0
	pushl $\vector
	jmp i386_trap_common
	.section .rodata.i386_isr_table, "a"
	.long i386_isr_\vector
	.text
.endm

/* Vector where the CPU has already pushed an error code. */
.macro ISR_ERR vector
	.text
	.p2align 4
i386_isr_\vector:
	pushl $\vector
	jmp i386_trap_common
	.section .rodata.i386_isr_table, "a"
	.long i386_isr_\vector
	.text
.endm

/*
 * The table is emitted one entry per macro use, in use order, so the uses
 * below must stay in ascending vector order with no gaps.
 */
	.section .rodata.i386_isr_table, "a"
	.balign 4
	.global i386_isr_table
i386_isr_table:

	ISR_NOERR 0
	ISR_NOERR 1
	ISR_NOERR 2
	ISR_NOERR 3
	ISR_NOERR 4
	ISR_NOERR 5
	ISR_NOERR 6
	ISR_NOERR 7
	ISR_ERR 8
	ISR_NOERR 9
	ISR_ERR 10
	ISR_ERR 11
	ISR_ERR 12
	ISR_ERR 13
	ISR_ERR 14
	ISR_NOERR 15
	ISR_NOERR 16
	ISR_ERR 17
	ISR_NOERR 18
	ISR_NOERR 19
	ISR_NOERR 20
	ISR_ERR 21
	ISR_NOERR 22
	ISR_NOERR 23
	ISR_NOERR 24
	ISR_NOERR 25
	ISR_NOERR 26
	ISR_NOERR 27
	ISR_NOERR 28
	ISR_ERR 29
	ISR_ERR 30
	ISR_NOERR 31

	ISR_NOERR 32
	ISR_NOERR 33
	ISR_NOERR 34
	ISR_NOERR 35
	ISR_NOERR 36
	ISR_NOERR 37
	ISR_NOERR 38
	ISR_NOERR 39
	ISR_NOERR 40
	ISR_NOERR 41
	ISR_NOERR 42
	ISR_NOERR 43
	ISR_NOERR 44
	ISR_NOERR 45
	ISR_NOERR 46
	ISR_NOERR 47

	.text
	.p2align 4
	.global i386_isr_syscall
i386_isr_syscall:
	pushl $0
	pushl $0x80
	jmp i386_trap_common

	.text
	.p2align 4
	.global i386_isr_ipi_reschedule
i386_isr_ipi_reschedule:
	pushl $0
	pushl $0xF0
	jmp i386_trap_common

	.text
	.p2align 4
	.global i386_isr_ipi_cpu_stop
i386_isr_ipi_cpu_stop:
	pushl $0
	pushl $0xF1
	jmp i386_trap_common

	.text
	.p2align 4
	.global i386_isr_ipi_tick
i386_isr_ipi_tick:
	pushl $0
	pushl $0xF2
	jmp i386_trap_common

	.text
	.p2align 4
	.global i386_isr_lapic_spurious
i386_isr_lapic_spurious:
	/* Spurious by architectural definition: no EOI, nothing to acknowledge. */
	pushl $0
	pushl $0xFF
	jmp i386_trap_common

/*
 * Common trap path. On entry the stack holds, from the top: trapno, err and
 * the hardware frame (eip, cs, efl [, uesp, ss]).
 */
	.p2align 4
i386_trap_common:
	pushal
	pushl %ds
	pushl %es
	pushl %fs
	pushl %gs

	/* The pushal slot for esp (offset 28) carries cr2 instead. */
	movl %cr2, %eax
	movl %eax, 28(%esp)

	/*
	 * Trap from ring 3 arrives with user data selectors loaded (%fs
	 * included: the kernel's percpu segment is not the user's to see).
	 * %fs specifically gets the percpu selector, not KERNEL_DATA_SEL --
	 * see gdt.h's file comment and GDT_PERCPU_SEL -- so machine_cpu_id()
	 * (an %fs:0 read) is correct for the rest of this trap, including in
	 * an IPI or any other interrupt handler.
	 */
	movw $KERNEL_DATA_SEL, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %gs
	movw $KERNEL_PERCPU_SEL, %ax
	movw %ax, %fs
	cld

	pushl %esp
	call i386_trap_handler
	addl $4, %esp

	popl %gs
	popl %fs
	popl %es
	popl %ds
	popal
	addl $8, %esp
	iret
