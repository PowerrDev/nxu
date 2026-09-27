/*
 * appkit_call_on_stack(entry, stack_top): run entry() on another stack
 * (cdecl: both arguments on the caller's stack). %ebp (callee-saved)
 * remembers the caller's stack across the call.
 */
.text
.global appkit_call_on_stack
.type appkit_call_on_stack, @function
appkit_call_on_stack:
	pushl %ebp
	movl %esp, %ebp
	movl 8(%ebp), %eax
	movl 12(%ebp), %esp
	call *%eax
	movl %ebp, %esp
	popl %ebp
	ret
.size appkit_call_on_stack, . - appkit_call_on_stack
