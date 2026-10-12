/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/start.S
 *
 * i386 kernel entry. A Multiboot loader (QEMU -kernel, GRUB) enters at _start
 * in 32-bit protected mode with paging off, %eax holding the Multiboot magic
 * and %ebx the physical address of the boot-information structure. The image
 * is loaded at physical 1 MiB but linked at 0xC0100000 (see
 * makedefs/linker-i386.ld), so the entry stub below is the only code that
 * runs at its physical address; it is linked into the low .boot section.
 *
 * Boot sequence:
 *
 *   1. Zero .bss (through its physical alias; the boot page directory and the
 *      boot stack live there).
 *   2. Fill i386_boot_pd with 2 MiB (PSE) pages: physical [0, 640 MiB) at
 *      0xC0000000 (the kernel direct map, which the VM layer keeps for good),
 *      and physical [0, 2 MiB) again at virtual 0 so the instruction after the
 *      paging switch still resolves.
 *   3. Enable PSE, load CR3, set CR0.PG | CR0.WP, jump to the higher half.
 *   4. Switch to the higher-half stack, let i386_boot_relocate() rebase the
 *      Multiboot structure onto the direct map and drop the identity mapping,
 *      then enter C at i386_init().
 *
 * The page directory this builds is the master kernel page directory: the VM
 * layer refines it in place (see pmap.c) rather than replacing it.
 */

#define MULTIBOOT_HEADER_MAGIC 0x1BADB002
/* Bit 0: align modules on pages. Bit 1: provide memory information. */
#define MULTIBOOT_HEADER_FLAGS 0x00000003

/* Must match KERNEL_VIRT_OFFSET in the linker script and VMM_HIGHER_HALF_BASE. */
#define KERNEL_VIRT_OFFSET 0xC0000000
#define PHYS(symbol) ((symbol) - KERNEL_VIRT_OFFSET)

#define PDE_PRESENT 0x001
#define PDE_WRITE 0x002
#define PDE_LARGE 0x080

#define CR4_PSE 0x00000010
#define CR0_WP 0x00010000
#define CR0_PG 0x80000000

/* Direct map size (VM_DIRECT_MAP_SIZE, 640 MiB) in 2 MiB directory entries. */
#define DIRECT_MAP_PDES 320
#define KERNEL_FIRST_PDE 1536

.section .multiboot, "a"
.balign 4

	.long MULTIBOOT_HEADER_MAGIC
	.long MULTIBOOT_HEADER_FLAGS
	.long -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

.section .text.boot, "ax"
.global _start
.type _start, @function

.extern __bss_start
.extern __bss_end

_start:
	cli
	cld

	/* %esi keeps the Multiboot magic and %ebx the info pointer throughout. */
	movl %eax, %esi

	movl $PHYS(__bss_start), %edi
	movl $PHYS(__bss_end), %ecx
	subl %edi, %ecx
	shrl $2, %ecx
	xorl %eax, %eax
	rep stosl

	/* PAE is required. Preserve the loader pointer across CPUID/MSR setup. */
	movl %ebx, %ebp
	movl $1, %eax
	cpuid
	testl $(1 << 6), %edx
	jz .Lno_pae
	movl $0x80000000, %eax
	cpuid
	cmpl $0x80000001, %eax
	jb .Lno_nx
	movl $0x80000001, %eax
	cpuid
	testl $(1 << 20), %edx
	jz .Lno_nx
	movl $0xc0000080, %ecx
	rdmsr
	orl $(1 << 11), %eax
	wrmsr
	movl $1, PHYS(i386_boot_nx)
.Lno_nx:
	movl %ebp, %ebx

	/* Four contiguous directory pages. PDPT entries have only Present. */
	movl $PHYS(i386_boot_pdpt), %edi
	movl $PHYS(i386_boot_pd), %eax
	xorl %ecx, %ecx
0:
	movl %eax, %edx
	orl $PDE_PRESENT, %edx
	movl %edx, (%edi, %ecx, 8)
	addl $4096, %eax
	incl %ecx
	cmpl $4, %ecx
	jb 0b

	movl $PHYS(i386_boot_pd), %edi
	xorl %ecx, %ecx
1:
	movl %ecx, %eax
	shll $21, %eax
	orl $(PDE_PRESENT | PDE_WRITE | PDE_LARGE), %eax
	movl %eax, (KERNEL_FIRST_PDE * 8)(%edi, %ecx, 8)
	incl %ecx
	cmpl $DIRECT_MAP_PDES, %ecx
	jb 1b
	/* Temporary low alias covers the physical entry stub. */
	movl $(PDE_PRESENT | PDE_WRITE | PDE_LARGE), (%edi)
	movl %cr4, %eax
	orl $(CR4_PSE | 0x20), %eax
	movl %eax, %cr4
	movl $PHYS(i386_boot_pdpt), %eax
	movl %eax, %cr3

	movl %cr0, %eax
	orl $(CR0_PG | CR0_WP), %eax
	movl %eax, %cr0

	movl $i386_start_high, %eax
	jmp *%eax

.Lno_pae:
	/* COM1 diagnostic before a stack or C runtime exists. */
	movw $0x3fb, %dx
	movb $0x80, %al
	outb %al, %dx
	movw $0x3f8, %dx
	movb $1, %al
	outb %al, %dx
	incw %dx
	xorb %al, %al
	outb %al, %dx
	movw $0x3fb, %dx
	movb $3, %al
	outb %al, %dx
	movl $.Lpae_message, %esi
.Lpae_print:
	lodsb
	testb %al, %al
	jz .Lhalt
	movb %al, %bl
.Lpae_uart_wait:
	movw $0x3fd, %dx
	inb %dx, %al
	testb $0x20, %al
	jz .Lpae_uart_wait
	movw $0x3f8, %dx
	movb %bl, %al
	outb %al, %dx
	jmp .Lpae_print
.Lpae_message:
	.asciz "i386_start: CPU does not support required PAE paging\r\n"
.Lhalt:
	cli
	hlt
	jmp .Lhalt


.size _start, . - _start

.section .text, "ax"
.global i386_start_high
.type i386_start_high, @function

i386_start_high:
	movl $i386_boot_stack_top, %esp
	xorl %ebp, %ebp

	/*
	 * i386_boot_relocate(magic, info): 8 bytes of padding keep %esp
	 * 16-aligned at the call. Returns the direct-map address of the
	 * Multiboot structure (or 0 if the loader was not Multiboot).
	 */
	subl $8, %esp
	pushl %ebx
	pushl %esi
	call i386_boot_relocate
	addl $16, %esp

	/* i386_init(magic, info). */
	subl $8, %esp
	pushl %eax
	pushl %esi
	call i386_init

	cli
	hlt
	jmp .

.size i386_start_high, . - i386_start_high

.section .bss.boot_pd, "aw", @nobits
.balign 4096

/* The master kernel page directory; see pmap.c. */
.global i386_boot_pd

i386_boot_pd:
	.skip 16384
.balign 4096
.global i386_boot_pdpt
i386_boot_pdpt:
	.skip 4096
.global i386_boot_nx
i386_boot_nx:
	.skip 4

.section .bss.stack, "aw", @nobits
.balign 4096

/*
 * One page below the stack that pmap_init() unmaps, so running off the
 * bottom of the boot stack faults (and reaches the double-fault task, which
 * has a stack of its own) instead of silently corrupting whatever precedes it.
 */
.global i386_boot_stack_guard

i386_boot_stack_guard:
	.skip 4096

/* Exported so the trap code can bound its backtrace to the boot stack. */
.global i386_boot_stack_bottom
.global i386_boot_stack_top

i386_boot_stack_bottom:
	.skip 16384

i386_boot_stack_top:
