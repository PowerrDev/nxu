/*
 * appkit_call_on_stack(entry, stack_top): run entry() on another stack.
 * x29 (callee-saved) remembers the caller's stack across the call.
 */
.text
.global appkit_call_on_stack
.type appkit_call_on_stack, %function
appkit_call_on_stack:
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	mov sp, x1
	blr x0
	mov sp, x29
	ldp x29, x30, [sp], #16
	ret
.size appkit_call_on_stack, . - appkit_call_on_stack
