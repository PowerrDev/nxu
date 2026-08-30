.section .text.vectors, "ax"
.balign 2048

.global exception_vectors
.global exception_vector_common

/* Keep these offsets synchronized with arm64_exception_frame_t. */
.equ EXCEPTION_FRAME_SIZE, 304
.equ EXCEPTION_FRAME_X16, 128
.equ EXCEPTION_FRAME_SP, 248
.equ EXCEPTION_FRAME_ELR, 256
.equ EXCEPTION_FRAME_SPSR, 264
.equ EXCEPTION_FRAME_FAR, 272
.equ EXCEPTION_FRAME_ESR, 280
.equ EXCEPTION_FRAME_VECTOR, 288
.equ EXCEPTION_FRAME_SP_EL0, 296

.macro VECTOR_ENTRY number
	sub sp, sp, #EXCEPTION_FRAME_SIZE
	str x16, [sp, #EXCEPTION_FRAME_X16]

	mov x16, #\number
	str x16, [sp, #EXCEPTION_FRAME_VECTOR]

	b exception_vector_common

	.space 0x80 - 20
.endm

exception_vectors:
	/* Current EL using SP_EL0. */
	VECTOR_ENTRY 0
	VECTOR_ENTRY 1
	VECTOR_ENTRY 2
	VECTOR_ENTRY 3

	/* Current EL using SP_ELx. */
	VECTOR_ENTRY 4
	VECTOR_ENTRY 5
	VECTOR_ENTRY 6
	VECTOR_ENTRY 7

	/* Lower EL using AArch64. */
	VECTOR_ENTRY 8
	VECTOR_ENTRY 9
	VECTOR_ENTRY 10
	VECTOR_ENTRY 11

	/* Lower EL using AArch32. */
	VECTOR_ENTRY 12
	VECTOR_ENTRY 13
	VECTOR_ENTRY 14
	VECTOR_ENTRY 15

exception_vector_common:
	/* Save x0-x15. */
	stp x0,  x1,  [sp, #0]
	stp x2,  x3,  [sp, #16]
	stp x4,  x5,  [sp, #32]
	stp x6,  x7,  [sp, #48]
	stp x8,  x9,  [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]

	/* x16 was saved by the vector entry before it stored the vector ID. */
	stp x17, x18, [sp, #136]
	stp x19, x20, [sp, #152]
	stp x21, x22, [sp, #168]
	stp x23, x24, [sp, #184]
	stp x25, x26, [sp, #200]
	stp x27, x28, [sp, #216]
	stp x29, x30, [sp, #232]

	/* Save the active exception-level stack before the frame allocation. */
	add x16, sp, #EXCEPTION_FRAME_SIZE
	str x16, [sp, #EXCEPTION_FRAME_SP]

	/* Save the architectural exception state. */
	mrs x16, ELR_EL1
	str x16, [sp, #EXCEPTION_FRAME_ELR]

	mrs x16, SPSR_EL1
	str x16, [sp, #EXCEPTION_FRAME_SPSR]

	mrs x16, FAR_EL1
	str x16, [sp, #EXCEPTION_FRAME_FAR]

	mrs x16, ESR_EL1
	str x16, [sp, #EXCEPTION_FRAME_ESR]

	/* SP_EL0 is the interrupted user stack for lower-EL entries. */
	mrs x16, SP_EL0
	str x16, [sp, #EXCEPTION_FRAME_SP_EL0]

	/* First C argument: pointer to the saved exception frame. */
	mov x0, sp
	bl exception_handle

	/*
	 * Recoverable handlers may modify these frame fields. Publish them back
	 * to the architectural return state before restoring general registers.
	 */
	ldr x16, [sp, #EXCEPTION_FRAME_ELR]
	msr ELR_EL1, x16

	ldr x16, [sp, #EXCEPTION_FRAME_SPSR]
	msr SPSR_EL1, x16

	/*
	 * Slots 0-3 are already running on SP_EL0, so changing SP_EL0 here
	 * would move the live frame before its registers were restored. For
	 * current-EL SPx and lower-EL entries, SP_EL1 owns this frame and the
	 * saved SP_EL0 may be restored safely.
	 */
	ldr x17, [sp, #EXCEPTION_FRAME_VECTOR]
	cmp x17, #4
	b.lo 1f

	ldr x16, [sp, #EXCEPTION_FRAME_SP_EL0]
	msr SP_EL0, x16
1:

	/* Restore x0-x15. */
	ldp x0,  x1,  [sp, #0]
	ldp x2,  x3,  [sp, #16]
	ldp x4,  x5,  [sp, #32]
	ldp x6,  x7,  [sp, #48]
	ldp x8,  x9,  [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]

	/* Restore x17-x30. */
	ldp x17, x18, [sp, #136]
	ldp x19, x20, [sp, #152]
	ldp x21, x22, [sp, #168]
	ldp x23, x24, [sp, #184]
	ldp x25, x26, [sp, #200]
	ldp x27, x28, [sp, #216]
	ldp x29, x30, [sp, #232]

	/* Restore x16 last because the vector entry used it temporarily. */
	ldr x16, [sp, #EXCEPTION_FRAME_X16]

	add sp, sp, #EXCEPTION_FRAME_SIZE
	eret
