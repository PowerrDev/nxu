.section .text.boot, "ax"
.global _start
.type _start, %function

.extern __bss_start
.extern __bss_end

_start:
	/* Mask all exceptions during early boot. */
	msr DAIFSet, #0xf

	/* Use SP_EL1. The stack lives in .bss and is installed after clearing. */
	msr SPSel, #1
	isb

	/*
	 * The ELF is linked in the higher half, but these instructions initially
	 * execute at the physical load alias with the MMU disabled. ADRP/ADD uses
	 * the link-time section delta, so the runtime result is the matching low
	 * physical alias while we remain in the bootstrap mapping.
	 *
	 * x0 contains QEMU's DTB address and must remain untouched.
	 */
	adrp x1, __bss_start
	add x1, x1, :lo12:__bss_start
	adrp x2, __bss_end
	add x2, x2, :lo12:__bss_end

.Lclear_bss:
	cmp x1, x2
	b.hs .Lbss_cleared

	str xzr, [x1], #8
	b .Lclear_bss

.Lbss_cleared:
	dsb sy

	/* Install the physical alias of the bootstrap stack. */
	adrp x1, stack_top
	add x1, x1, :lo12:stack_top
	mov sp, x1

	/* x0 still contains the DTB address supplied by QEMU. */
	bl kern_init

.Lhalt:
	wfe
	b .Lhalt

.size _start, . - _start

.section .bss.stack, "aw", %nobits
.balign 16

stack_bottom:
	.skip 16384

stack_top:
