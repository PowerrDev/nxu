.section .text, "ax"
.align 4

/* Keep these offsets synchronized with arm64_kernel_context_t. */
.equ ARM64_CONTEXT_X19, 0
.equ ARM64_CONTEXT_X21, 16
.equ ARM64_CONTEXT_X23, 32
.equ ARM64_CONTEXT_X25, 48
.equ ARM64_CONTEXT_X27, 64
.equ ARM64_CONTEXT_FP, 80
.equ ARM64_CONTEXT_SP, 96

.global arm64_switch_context
.type arm64_switch_context, %function

/*
 * arm64_switch_context
 *
 * x0: old arm64_kernel_context_t
 * x1: new arm64_kernel_context_t
 *
 * Save the AArch64 callee-saved kernel context, replace SP_EL1, restore the
 * incoming context and return through its x30. Caller-saved x0-x18 are not
 * part of a normal kernel context switch; asynchronous architectural state
 * is preserved separately by arm64_exception_frame_t.
 */
arm64_switch_context:
	stp x19, x20, [x0, #ARM64_CONTEXT_X19]
	stp x21, x22, [x0, #ARM64_CONTEXT_X21]
	stp x23, x24, [x0, #ARM64_CONTEXT_X23]
	stp x25, x26, [x0, #ARM64_CONTEXT_X25]
	stp x27, x28, [x0, #ARM64_CONTEXT_X27]
	stp x29, x30, [x0, #ARM64_CONTEXT_FP]

	mov x2, sp
	str x2, [x0, #ARM64_CONTEXT_SP]

	ldp x19, x20, [x1, #ARM64_CONTEXT_X19]
	ldp x21, x22, [x1, #ARM64_CONTEXT_X21]
	ldp x23, x24, [x1, #ARM64_CONTEXT_X23]
	ldp x25, x26, [x1, #ARM64_CONTEXT_X25]
	ldp x27, x28, [x1, #ARM64_CONTEXT_X27]
	ldp x29, x30, [x1, #ARM64_CONTEXT_FP]

	ldr x2, [x1, #ARM64_CONTEXT_SP]
	mov sp, x2

	ret

.size arm64_switch_context, . - arm64_switch_context
