/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/smp_trampoline.S
 *
 * The code a secondary CPU runs from INIT-SIPI-SIPI (kern/i386/apic.c) to
 * the moment it calls into C. Compiled and linked as an ordinary part of the
 * kernel image (its higher-half .text address is irrelevant and never used),
 * but at *runtime* smp.c copies the bytes between i386_smp_trampoline_start
 * and i386_smp_trampoline_end verbatim to physical SMP_TRAMPOLINE_PHYS
 * (0x8000, see smp.h) before starting a CPU: real mode's SIPI vector can only
 * address a page-aligned address below 1 MiB, so the code has to actually
 * live there when the new CPU's PC first fetches from it, not just be
 * reachable through paging the way everything else in the kernel is.
 *
 * That copy is why every jump target here is computed as
 * "SMP_TRAMPOLINE_PHYS + (label - i386_smp_trampoline_start)", a compile-time
 * constant GAS can evaluate from the label difference alone, instead of the
 * label's own linked address: wherever the linker actually placed this
 * code, the *copy* always starts at SMP_TRAMPOLINE_PHYS, and only offsets
 * relative to that copy's own start mean anything once it is running from
 * there. Near jumps within one .code16 or .code32 block do not have this
 * problem (they are already relative), only the one far jump between them
 * does.
 *
 * smp_trampoline_params_t (smp.h) sits at SMP_TRAMPOLINE_PARAMS (0x8800, a
 * fixed offset into the same copied page): smp.c fills it in through the
 * direct map before triggering INIT-SIPI-SIPI, and every field this code
 * reads is read by its absolute physical address, which stays valid
 * throughout -- real mode addresses physically anyway, and PDE[0] (see
 * kern/i386/smp.c) keeps physical [0, 4 MiB) identity-mapped for exactly as
 * long as any CPU might still be running this file, protected mode included.
 *
 * Sequence: 16-bit real mode (GDT from the params block, CR0.PE) -> far jump
 * into 32-bit protected mode, still physically addressed (CR3, CR4.PSE,
 * CR0.PG) -> an ordinary near jump to this CPU's real, linked, higher-half
 * .text address, the same trick start.S's own i386_start_high uses -> load
 * this CPU's own stack and call i386_smp_secondary_main(cpu_id, processor),
 * both read from the params block, which never returns.
 */

#define SMP_TRAMPOLINE_PHYS 0x8000
#define SMP_TRAMPOLINE_PARAMS 0x8800

/* Field offsets into smp_trampoline_params_t (smp.h); keep in sync. */
#define PARAMS_GDT_LIMIT (SMP_TRAMPOLINE_PARAMS + 0)
#define PARAMS_CR3 (SMP_TRAMPOLINE_PARAMS + 6)
#define PARAMS_STACK_TOP (SMP_TRAMPOLINE_PARAMS + 10)
#define PARAMS_CPU_ID (SMP_TRAMPOLINE_PARAMS + 14)
#define PARAMS_PROCESSOR_PTR (SMP_TRAMPOLINE_PARAMS + 18)

#define CR4_PSE 0x00000010
#define CR0_PE 0x00000001
#define CR0_WP 0x00010000
#define CR0_PG 0x80000000

#define KERNEL_CODE_SEL 0x08
#define KERNEL_DATA_SEL 0x10

.section .text, "ax"

.global i386_smp_trampoline_start
.global i386_smp_trampoline_end

.code16
.balign 16
i386_smp_trampoline_start:
	cli
	cld

	xorw %ax, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss

	/* A GDTR: 6 bytes, {limit:16, base:32}, exactly smp_trampoline_params_t's first field -- lgdt reads it straight from the params block. */
	lgdtl PARAMS_GDT_LIMIT

	movl %cr0, %eax
	orl $CR0_PE, %eax
	movl %eax, %cr0

	ljmpl $KERNEL_CODE_SEL, $(SMP_TRAMPOLINE_PHYS + (i386_smp_trampoline_32 - i386_smp_trampoline_start))

.code32
i386_smp_trampoline_32:
	movw $KERNEL_DATA_SEL, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss

	movl PARAMS_CR3, %eax
	movl %eax, %cr3

	movl %cr4, %eax
	orl $CR4_PSE, %eax
	movl %eax, %cr4

	movl %cr0, %eax
	orl $(CR0_PG | CR0_WP), %eax
	movl %eax, %cr0

	/*
	 * Paging is on. i386_smp_trampoline_high below is an ordinary part of
	 * the kernel image with a real, linked, higher-half address -- this is
	 * the one jump in the whole file that is NOT relative to
	 * SMP_TRAMPOLINE_PHYS, exactly like start.S's own jump to
	 * i386_start_high, and for the same reason: everything from here on
	 * runs through the shared kernel page tables like any other kernel
	 * code, so its real address is the only one that means anything.
	 */
	movl $i386_smp_trampoline_high, %eax
	jmp *%eax

.size i386_smp_trampoline_32, . - i386_smp_trampoline_32
i386_smp_trampoline_end:

.section .text, "ax"
.p2align 4
.type i386_smp_trampoline_high, @function
i386_smp_trampoline_high:
	movl PARAMS_STACK_TOP, %esp
	xorl %ebp, %ebp

	movl PARAMS_PROCESSOR_PTR, %eax
	pushl %eax
	movl PARAMS_CPU_ID, %eax
	pushl %eax
	call i386_smp_secondary_main

.Lhang:
	cli
	hlt
	jmp .Lhang

.size i386_smp_trampoline_high, . - i386_smp_trampoline_high

.section .note.GNU-stack, "", @progbits
