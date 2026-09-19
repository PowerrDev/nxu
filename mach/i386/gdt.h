/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/gdt.h
 *
 * Global descriptor table and task-state segments for 32-bit x86.
 *
 * Segmentation is flat (base 0, 4 GiB limit) and exists only because the
 * hardware requires it: it supplies the ring 0 / ring 3 privilege split and
 * the TSS the CPU consults for the kernel stack on a user-to-kernel trap.
 * Two TSSes are kept: the main one, and one reserved for the double-fault
 * task gate so #DF always runs on a known-good stack.
 */

#ifndef NXU_MACH_I386_GDT_H
#define NXU_MACH_I386_GDT_H

#include <stdint.h>

#if !defined(__i386__)
#error "mach/i386/gdt.h: the x86_64 GDT/TSS layout is not implemented yet"
#endif

/*
 * Selector values. The kernel selectors are plain tokens so they can be
 * stringified into inline assembly; the user selectors carry RPL 3.
 */
#define GDT_KERNEL_CODE_SEL 0x08
#define GDT_KERNEL_DATA_SEL 0x10
#define GDT_USER_CODE_SEL 0x1B
#define GDT_USER_DATA_SEL 0x23
#define GDT_TSS_SEL 0x28
#define GDT_DF_TSS_SEL 0x30

/* A present-bit-clear slot inside the GDT limit, kept for #NP testing. */
#define GDT_SPARE_SEL 0x38

/* 32-bit task-state segment: 104 bytes, fixed by the architecture. */
typedef struct __attribute__((packed)) {
	uint32_t link;
	uint32_t esp0;
	uint32_t ss0;
	uint32_t esp1;
	uint32_t ss1;
	uint32_t esp2;
	uint32_t ss2;
	uint32_t cr3;
	uint32_t eip;
	uint32_t eflags;
	uint32_t eax;
	uint32_t ecx;
	uint32_t edx;
	uint32_t ebx;
	uint32_t esp;
	uint32_t ebp;
	uint32_t esi;
	uint32_t edi;
	uint32_t es;
	uint32_t cs;
	uint32_t ss;
	uint32_t ds;
	uint32_t fs;
	uint32_t gs;
	uint32_t ldt;
	uint16_t trap;
	uint16_t iomap_base;
} i386_tss_t;

_Static_assert(sizeof(i386_tss_t) == 104U, "i386_tss_t must match the hardware TSS layout");

/* Build the GDT and both TSSes, reload every segment register and load TR. */
void i386_gdt_init(void);

/* Stack the CPU switches to when a trap arrives from ring 3. */
void i386_gdt_set_kernel_stack(uint32_t esp0);

/*
 * Page directory the double-fault task runs on. The task switch loads CR3
 * from the TSS, so it must be a directory that maps the kernel (any address
 * space's kernel half will do, the master kernel directory is the safe pick).
 */
void i386_gdt_set_double_fault_cr3(uint32_t cr3);

/*
 * The main TSS. After a task switch to the double-fault TSS the CPU stores
 * the faulting context here, which is what the #DF report reads.
 */
const i386_tss_t *i386_gdt_main_tss(void);

#endif
