/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/context_switch.S
 *
 * Kernel context switch for 32-bit x86, the counterpart of
 * mach/arm64/context_switch.S. cdecl: the caller has already saved whatever
 * it wants of eax, ecx and edx; only ebp, ebx, esi and edi (and esp) must
 * survive, so those are all that is saved. Full interrupted state stays the
 * job of x86_saved_state32_t.
 */

	.section .text, "ax"
	.p2align 4

	.global i386_switch_context
	.type i386_switch_context, @function

/*
 * void i386_switch_context(uint32_t *old_sp, uint32_t new_sp)
 *
 *   4(%esp)  old_sp: where to store the outgoing kernel stack pointer
 *   8(%esp)  new_sp: the incoming kernel stack pointer
 *
 * The incoming stack holds, from new_sp upwards: edi, esi, ebx, ebp and the
 * return address. machine_thread_prepare_context() fabricates the same
 * frame for a thread that has never run.
 */
i386_switch_context:
	movl 4(%esp), %eax
	movl 8(%esp), %edx

	pushl %ebp
	pushl %ebx
	pushl %esi
	pushl %edi

	movl %esp, (%eax)
	movl %edx, %esp

	popl %edi
	popl %esi
	popl %ebx
	popl %ebp

	ret

	.size i386_switch_context, . - i386_switch_context

	.section .note.GNU-stack, "", @progbits
