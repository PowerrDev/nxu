/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/arm64/smp_entry.S
 *
 * Where a secondary CPU starts. PSCI CPU_ON hands the CPU this code's
 * physical address and one argument (in x0), with the MMU and caches off, at
 * EL1, all interrupts masked and no stack.
 *
 * It lives in .text.boot, which the linker script places first in the image,
 * so it is inside the two pages the boot CPU identity-maps for it
 * (kern/arm64/smp.c): the first instruction after the MMU comes on is still
 * fetched from this physical address, and must translate to itself.
 *
 * Everything the CPU needs comes from the smp_boot_params_t block x0 points
 * at (a physical address; the layout is fixed by the ARM64_SMP_* offsets and
 * checked against the C structure in smp.c). The boot CPU filled it with
 * its own live MMU configuration, so every CPU runs the same translation
 * regime.
 */

.equ ARM64_SMP_MAIR,     0
.equ ARM64_SMP_TCR,      8
.equ ARM64_SMP_TTBR0,    16
.equ ARM64_SMP_TTBR1,    24
.equ ARM64_SMP_SCTLR,    32
.equ ARM64_SMP_STACK,    40
.equ ARM64_SMP_ENTRY,    48
.equ ARM64_SMP_VBAR,     56
.equ ARM64_SMP_CPU,      64

.equ SCTLR_ICACHE, (1 << 12)

.section .text.boot, "ax"
.balign 16
.global arm64_secondary_entry
.type arm64_secondary_entry, %function

arm64_secondary_entry:
	/* Interrupts stay masked until the CPU has its own vector table and stack. */
	msr DAIFSet, #0xf
	msr SPSel, #1
	isb

	/* x0: physical address of this CPU's boot parameters (PSCI context_id). */
	ldr x1, [x0, #ARM64_SMP_MAIR]
	ldr x2, [x0, #ARM64_SMP_TCR]
	ldr x3, [x0, #ARM64_SMP_TTBR0]
	ldr x4, [x0, #ARM64_SMP_TTBR1]
	ldr x5, [x0, #ARM64_SMP_SCTLR]
	ldr x6, [x0, #ARM64_SMP_STACK]
	ldr x7, [x0, #ARM64_SMP_ENTRY]
	ldr x8, [x0, #ARM64_SMP_VBAR]
	ldr x0, [x0, #ARM64_SMP_CPU]

	msr MAIR_EL1, x1
	msr TCR_EL1, x2
	msr TTBR0_EL1, x3
	msr TTBR1_EL1, x4
	isb

	/*
	 * This CPU's own TLB is not guaranteed empty out of reset; drop it before
	 * the MMU is on. Non-shareable is right: nobody else has this CPU's
	 * translations. Same for the instruction cache before it is enabled.
	 */
	tlbi vmalle1
	ic iallu
	dsb nsh
	isb

	/* The MMU (and whatever cache bits the boot CPU runs with) come on here. */
	msr SCTLR_EL1, x5
	isb

	/* Now on TTBR0's identity map for this page: install the higher-half vectors and stack, and go. */
	msr VBAR_EL1, x8
	mov sp, x6
	isb
	br x7

.size arm64_secondary_entry, . - arm64_secondary_entry
