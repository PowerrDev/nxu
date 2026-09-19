/*
 * i386 process entry. The loader starts a process at _start with
 * esp = VM_USER_STACK_TOP (16-byte aligned, one past the top of the stack
 * mapping), every general register zero and no arguments. main() takes none
 * either, exactly as on arm64, so _start just calls it and exits with its
 * return value.
 *
 * NXU_SYS_EXIT is 1, status in ebx (see frameworks/lib/syscall_i386.c for the
 * whole convention). exit does not return; the trailing loop is only there so
 * a kernel that did return would not run off the end of .text.
 */
.section .text.start,"ax",@progbits
.global _start
.type _start, @function
_start:
	xorl %ebp, %ebp
	andl $-16, %esp
	call main
	movl %eax, %ebx
	movl $1, %eax
	int $0x80
1:
	pause
	jmp 1b
.size _start, . - _start
