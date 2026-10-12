/* Controlled fetch-fault probe. A failed CALL has already pushed its return PC. */

.section .text, "ax"
.global i386_vm_probe_execute
.type i386_vm_probe_execute, @function

i386_vm_probe_execute:
	movl 4(%esp), %eax
	movl %eax, i386_vm_fault_expectation+4
	movl %eax, i386_vm_fault_expectation+8
	movl $.Lfetch_resume, i386_vm_fault_expectation+12
	movl $0, i386_vm_fault_expectation+16
	movl $0, i386_vm_fault_expectation+20
	movl $1, i386_vm_fault_expectation
	call *%eax
	jmp .Lfetch_done

.Lfetch_resume:
	addl $4, %esp

.Lfetch_done:
	movl $0, i386_vm_fault_expectation
	ret
.size i386_vm_probe_execute, . - i386_vm_probe_execute

.section .note.GNU-stack, "", @progbits
