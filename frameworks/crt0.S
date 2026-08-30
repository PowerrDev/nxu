.section .text.start,"ax"
.global _start
.type _start, %function
_start:
	bl main
	mov x8, #1
	svc #0
1:
	wfe
	b 1b
.size _start, . - _start
