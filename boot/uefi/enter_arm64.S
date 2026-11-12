/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        boot/uefi/enter_arm64.S
 *
 * The last steps of NBL on AArch64, after ExitBootServices. The kernel's
 * entry expects what a direct boot gives it: the MMU and the caches off,
 * the Device Tree address in x0. The firmware runs with both on and maps
 * memory one to one, so this code keeps executing across the switch.
 */

	.text
	.p2align 2

/*
 * void nbl_arm64_clean_range(uint64_t start, uint64_t end)
 *
 * Write the data cache lines of [start, end) back to memory, so what the
 * loader placed there is what the kernel reads once the cache is off.
 */
	.globl nbl_arm64_clean_range
nbl_arm64_clean_range:
	mrs x2, ctr_el0
	ubfx x2, x2, #16, #4
	mov x3, #4
	lsl x3, x3, x2
	sub x4, x3, #1
	bic x0, x0, x4
1:
	cmp x0, x1
	b.hs 2f
	dc cvac, x0
	add x0, x0, x3
	b 1b
2:
	dsb sy
	ret

/*
 * void nbl_arm64_enter(uint64_t entry, uint64_t device_tree)
 *
 * Mask exceptions, turn the MMU and both caches off at the current exception
 * level (SCTLR M, C and I, the state a CPU leaves reset in and the kernel's
 * cache bring-up starts from), and enter the kernel. Does not return.
 */
	.globl nbl_arm64_enter
nbl_arm64_enter:
	msr daifset, #0xf
	mov x19, x0
	mov x20, x1

	mrs x2, CurrentEL
	lsr x2, x2, #2
	cmp x2, #2
	b.eq 1f

	mrs x3, sctlr_el1
	bic x3, x3, #0x1
	bic x3, x3, #0x4
	bic x3, x3, #0x1000
	msr sctlr_el1, x3
	isb
	tlbi vmalle1
	b 2f
1:
	mrs x3, sctlr_el2
	bic x3, x3, #0x1
	bic x3, x3, #0x4
	bic x3, x3, #0x1000
	msr sctlr_el2, x3
	isb
	tlbi alle2
2:
	dsb sy
	isb
	ic iallu
	dsb sy
	isb

	mov x0, x20
	mov x1, xzr
	mov x2, xzr
	mov x3, xzr
	br x19
