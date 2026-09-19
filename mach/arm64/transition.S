.section .text, "ax"
.align 4

.global arm64_enter_higher_half
.type arm64_enter_higher_half, %function

/*
 * x0: higher-half offset
 * x1: higher-half VBAR_EL1
 * x2: higher-half continuation
 */
arm64_enter_higher_half:
	msr daifset, #0xf
	isb

	mov x9, sp
	add x9, x9, x0
	mov sp, x9

	msr vbar_el1, x1
	dsb sy
	isb

	br x2

.size arm64_enter_higher_half, . - arm64_enter_higher_half

.global arm64_enter_el0
.type arm64_enter_el0, %function

/*
 * x0: first EL0 instruction
 * x1: initial EL0 stack pointer
 * x2: initial SPSR_EL1 value
 */
arm64_enter_el0:
	/*
	 * Preserve the kernel callee-saved register set on SP_EL1. EL0 is not an
	 * AAPCS64 callee and may freely modify x19-x30 before trapping back into
	 * the kernel. The return trampoline restores the kernel values before C
	 * execution resumes.
	 */
	sub sp, sp, #96
	stp x19, x20, [sp, #0]
	stp x21, x22, [sp, #16]
	stp x23, x24, [sp, #32]
	stp x25, x26, [sp, #48]
	stp x27, x28, [sp, #64]
	stp x29, x30, [sp, #80]

	msr elr_el1, x0
	msr sp_el0, x1

	/*
	 * The owning thread supplies its initial EL0 PSTATE.
	 */
	msr spsr_el1, x2

	isb
	eret

.size arm64_enter_el0, . - arm64_enter_el0


.global arm64_el0_return_address
.type arm64_el0_return_address, %function

arm64_el0_return_address:
	/*
	 * The kernel is linked at its permanent higher-half VMA. ADR keeps this
	 * return target in the currently executing TTBR1 alias.
	 */
	adr x0, arm64_return_from_el0
	ret

.size arm64_el0_return_address, . - arm64_el0_return_address


.type arm64_return_from_el0, %function

arm64_return_from_el0:
	/*
	 * We arrive here through ERET using SP_EL1. Restore the kernel register
	 * set which was protected from arbitrary EL0 modifications.
	 */
	ldp x19, x20, [sp, #0]
	ldp x21, x22, [sp, #16]
	ldp x23, x24, [sp, #32]
	ldp x25, x26, [sp, #48]
	ldp x27, x28, [sp, #64]
	ldp x29, x30, [sp, #80]
	add sp, sp, #96
	ret

.size arm64_return_from_el0, . - arm64_return_from_el0
