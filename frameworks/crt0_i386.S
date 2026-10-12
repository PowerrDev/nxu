/*
 * i386 process entry. The loader starts a process at _start with
 * esp = VM_USER_STACK_TOP (16-byte aligned, one past the top of the stack
 * mapping), every general register zero and no arguments. Normal userland
 * builds bootstrap shell arguments and stdio over a local socket before
 * calling main; fixture builds keep the original zero-argument entry.
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
#if defined(NXU_SPAWN_IO_ENTRY)
	/* Bootstrap argv and redirected stdio for shell-spawned programs. */
	subl $16, %esp
	leal 12(%esp), %eax
	movl %eax, (%esp)
	call nxu_spawn_io_start
	movl 12(%esp), %ecx
	movl %eax, (%esp)
	movl %edx, 4(%esp)
	movl %ecx, 8(%esp)
#else
	subl $4, %esp
	pushl $0
	pushl $0
	pushl $0
#endif
	call main
	addl $16, %esp
	movl %eax, %ebx
	movl $1, %eax
	int $0x80
1:
	pause
	jmp 1b
.size _start, . - _start
