/* Position-independent long-mode -> i386 Multiboot handoff. Copied below 640 MiB. */
.text
.global nxu_transition_start,nxu_transition_end
.global nxu_transition_entry,nxu_transition_info,nxu_transition_stack

nxu_transition_start:

.code64
	cli
	leaq nxu_transition_start(%rip), %rbp
	leaq transition_gdt(%rip), %rax
	movq %rax, transition_gdtr+2(%rip)
	lgdt transition_gdtr(%rip)
	leaq transition_32(%rip), %rax
	pushq $8
	pushq %rax
	lretq

.code32

transition_32:
	movw $16, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss
	movl nxu_transition_stack-nxu_transition_start(%ebp), %esp
	/* PCID must be off before clearing CR0.PG. */
	movl %cr4, %eax
	andl $0xfffdffff, %eax
	movl %eax, %cr4
	movl %cr0, %eax
	andl $0x7fffffff, %eax
	movl %eax, %cr0
	movl $0xc0000080, %ecx
	rdmsr
	andl $0xfffffeff, %eax
	wrmsr
	movl %cr4, %eax
	/* Clear PAE and five-level paging for the non-PAE kernel. */
	andl $0xffffefdf, %eax
	movl %eax, %cr4
	movl nxu_transition_info-nxu_transition_start(%ebp), %ebx
	movl nxu_transition_entry-nxu_transition_start(%ebp), %edx
	movl $0x2badb002, %eax
	jmp *%edx

.balign 8

transition_gdt:
	.quad 0
	.quad 0x00cf9a000000ffff
	.quad 0x00cf92000000ffff

transition_gdtr:
	.word 23
	.quad 0
nxu_transition_entry:
	.long 0
nxu_transition_info:
	.long 0
nxu_transition_stack:
	.long 0

nxu_transition_end:
