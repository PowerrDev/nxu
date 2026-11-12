/*
 * arm64 process entry. The loader starts every process at _start with sp
 * pointing at its arguments, laid out in 64-bit words:
 *
 *     sp ->  argc
 *            argv[0] ... argv[argc - 1]
 *            0
 *
 * whether it was spawned or reached by exec. A process started without
 * arguments finds argc 0. NXU_SYS_EXIT is 1, status in x0.
 */
.section .text.start,"ax"
.global _start
.type _start, %function
_start:
	ldr x0, [sp]
	add x1, sp, #8
	bl main
	mov x8, #1
	svc #0
1:
	wfe
	b 1b
.size _start, . - _start
