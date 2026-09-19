/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/transition.S
 *
 * Ring 0 <-> ring 3 transitions, the counterpart of mach/arm64/transition.S.
 *
 *   i386_enter_user     first entry of a thread into ring 3
 *   i386_user_return    where a terminated user program's trap frame is
 *                       redirected so i386_enter_user() "returns"
 */

/* Keep in step with mach/i386/gdt.h (thread.c asserts the selectors). */
#define USER_CODE_SEL 0x1B
#define USER_DATA_SEL 0x23

/* Keep in step with machine_thread_t in mach/i386/thread.h. */
#define MACHINE_USER_PC 4
#define MACHINE_USER_SP 8
#define MACHINE_USER_EFLAGS 12
#define MACHINE_USER_ARG 16
#define MACHINE_USER_KERNEL_SP 28

	.section .text, "ax"
	.p2align 4

	.global i386_enter_user
	.type i386_enter_user, @function

/*
 * void i386_enter_user(machine_thread_t *machine)
 *
 * The kernel is not a cdecl callee of ring 3: user code may change every
 * register before it traps back, and the CPU enters the kernel at TSS esp0
 * rather than at the current stack pointer. So the callee-saved registers
 * are parked on this stack, machine->user_kernel_sp remembers where, and
 * esp0 is set to the (16-byte aligned) address just below that, which keeps
 * every later trap frame clear of the parked context. i386_user_return
 * pops it again.
 *
 * Interrupts stay masked until the iret loads the user eflags.
 */
i386_enter_user:
	cli

	pushl %ebp
	pushl %ebx
	pushl %esi
	pushl %edi

	movl 20(%esp), %ebx
	movl %esp, MACHINE_USER_KERNEL_SP(%ebx)

	andl $-16, %esp

	/* i386_thread_user_entry_prepare(machine, esp0), with esp 16-aligned at the call. */
	movl %esp, %eax
	subl $8, %esp
	pushl %eax
	pushl %ebx
	call i386_thread_user_entry_prepare
	addl $16, %esp

	/* The iret frame: ss, esp, eflags, cs, eip. */
	pushl $USER_DATA_SEL
	pushl MACHINE_USER_SP(%ebx)
	pushl MACHINE_USER_EFLAGS(%ebx)
	pushl $USER_CODE_SEL
	pushl MACHINE_USER_PC(%ebx)

	/* eax is the initial argument; every other register starts at zero. */
	movl MACHINE_USER_ARG(%ebx), %eax

	movl $USER_DATA_SEL, %ecx
	movw %cx, %ds
	movw %cx, %es
	movw %cx, %fs
	movw %cx, %gs

	xorl %ecx, %ecx
	xorl %edx, %edx
	xorl %esi, %esi
	xorl %edi, %edi
	xorl %ebp, %ebp
	xorl %ebx, %ebx

	iret

	.size i386_enter_user, . - i386_enter_user


	.global i386_user_return
	.type i386_user_return, @function

/*
 * Entered by an iret from i386_trap_handler's frame with ring 0 selectors,
 * interrupts masked, and eax = the machine's user_kernel_sp. Unwinds the
 * context i386_enter_user parked and returns to its caller, exactly where
 * arm64_return_from_el0 returns on arm64. The stack we are on is discarded.
 */
i386_user_return:
	movl %eax, %esp

	popl %edi
	popl %esi
	popl %ebx
	popl %ebp

	ret

	.size i386_user_return, . - i386_user_return

	.section .note.GNU-stack, "", @progbits
