/*
 * i386 process entry. The loader starts every process at _start with esp
 * pointing at its arguments, laid out in 32-bit words:
 *
 *     esp ->  argc
 *             argv[0] ... argv[argc - 1]
 *             0
 *
 * and every general register but eax zero. A process started without
 * arguments finds argc 0. main is declared main(uint64_t argc, const char
 * *const *argv), so argc is passed as two words.
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
	movl (%esp), %eax
	leal 4(%esp), %edx
	andl $-16, %esp
	subl $4, %esp
	pushl %edx
	pushl $0
	pushl %eax
	call main
	addl $16, %esp
	movl %eax, %ebx
	movl $1, %eax
	int $0x80
1:
	pause
	jmp 1b
.size _start, . - _start
