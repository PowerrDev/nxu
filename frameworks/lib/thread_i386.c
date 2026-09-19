/*
 * i386 nxu_thread_spawn: the counterpart of frameworks/lib/thread.c.
 *
 * A thread made by nxu_thread_create starts with eip = the entry point and
 * esp = the stack pointer exactly as passed (nothing pushed, so there is no
 * return address and no argument slot), eax = arg and every other register
 * zero. The C entry point wants a cdecl frame, so the thread starts in
 * nxu_thread_trampoline instead. nxu_thread_spawn leaves the real entry and
 * its argument at the top of the new stack, where the trampoline finds them:
 *
 *       top    +-----------+
 *       top-4  |    arg    |
 *       top-8  |   entry   |  <-- initial esp
 *              +-----------+
 *              |  (stack   |
 *              |   grows   |
 *              |   down)   |
 *
 * The trampoline reads the pair, drops below it onto a 16-byte-aligned
 * frame, calls entry(arg) with a proper cdecl call, and if entry returns
 * calls nxu_thread_exit(0), so a thread body may simply return.
 */

#include <nxu/thread.h>

#include <nxu/syscall.h>

#include <stdint.h>

#if !defined(__i386__)
#error "frameworks/lib/thread_i386.c is the i386 implementation of nxu_thread_spawn"
#endif

void nxu_thread_trampoline(void *arg);

__asm__(
	".text\n"
	".p2align 4\n"
	".globl nxu_thread_trampoline\n"
	".type nxu_thread_trampoline, @function\n"
	"nxu_thread_trampoline:\n"
	"	xorl %ebp, %ebp\n"
	"	movl (%esp), %eax\n"
	"	movl 4(%esp), %ecx\n"
	"	andl $-16, %esp\n"
	"	subl $16, %esp\n"
	"	movl %ecx, (%esp)\n"
	"	call *%eax\n"
	"	movl $0, (%esp)\n"
	"	movl $0, 4(%esp)\n"
	"	call nxu_thread_exit\n"
	"	ud2\n"
	".size nxu_thread_trampoline, . - nxu_thread_trampoline\n"
);

int64_t
nxu_thread_spawn(void (*entry)(void *arg), void *arg, uint64_t stack_size)
{
	if (entry == 0 || stack_size < 64ULL || stack_size > 0x7FFFFFFFULL) return -NXU_SYS_E_INVALID_ARGUMENT;

	int64_t base = nxu_mmap(stack_size, NXU_MMAP_PROT_READ_WRITE);
	if (base < 0) return base;

	uintptr_t top = ((uintptr_t)base + (uintptr_t)stack_size) & ~(uintptr_t)15U;
	/*
	 * The kernel rejects a thread stack pointer that is not 16-byte aligned
	 * (syscall_thread_create, shared with arm64), so the {entry, arg} frame
	 * sits a full 16 bytes below the aligned top, not just the two words it
	 * needs. The trampoline re-aligns its own call frame regardless.
	 */
	uintptr_t *frame = (uintptr_t *)(top - 16U);

	frame[0] = (uintptr_t)entry;
	frame[1] = (uintptr_t)arg;

	return nxu_thread_create(nxu_thread_trampoline, frame, 0);
}
