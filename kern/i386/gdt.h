/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/gdt.h
 *
 * Global descriptor table and task-state segments for 32-bit x86.
 *
 * Segmentation is flat (base 0, 4 GiB limit) and exists only because the
 * hardware requires it: it supplies the ring 0 / ring 3 privilege split and
 * the TSS the CPU consults for the kernel stack on a user-to-kernel trap. And,
 * as of SMP, the one thing a flat model still needs a *non-flat* segment for:
 * a per-CPU base, since i386 has no register that just tells a CPU "which one
 * are you" the way arm64's TPIDR_EL1/MPIDR_EL1 do (see smp.h).
 *
 * Every CPU gets its own complete copy of this table (g_gdt[cpu_id]), its own
 * main and double-fault TSS, and its own one-word "percpu" segment -- not one
 * shared table with per-CPU descriptor *slots*, a genuinely separate table
 * per CPU. That is what makes a single fixed selector number (GDT_PERCPU_SEL,
 * loaded into %fs and never touched by i386_switch_context -- see
 * context_switch.S's comment) resolve to a different base on every CPU: the
 * selector is the same everywhere, but which table it is looked up in is
 * whichever GDTR that CPU itself loaded. The DF task gate works the same way:
 * the IDT is one shared table with one task gate pointing at GDT_DF_TSS_SEL,
 * and that selector is resolved against whichever CPU's GDT is current when
 * #DF fires, so each CPU double-faults onto its own stack automatically.
 */

#ifndef NXU_KERN_I386_GDT_H
#define NXU_KERN_I386_GDT_H

#include <stdint.h>

#if !defined(__i386__)
#error "kern/i386/gdt.h: the x86_64 GDT/TSS layout is not implemented yet"
#endif

/*
 * Selector values. The kernel selectors are plain tokens so they can be
 * stringified into inline assembly; the user selectors carry RPL 3. Identical
 * on every CPU (see the file comment): only the table they are resolved
 * against differs.
 */
#define GDT_KERNEL_CODE_SEL 0x08
#define GDT_KERNEL_DATA_SEL 0x10
#define GDT_USER_CODE_SEL 0x1B
#define GDT_USER_DATA_SEL 0x23
#define GDT_TSS_SEL 0x28
#define GDT_DF_TSS_SEL 0x30

/* A present-bit-clear slot inside the GDT limit, kept for #NP testing. */
#define GDT_SPARE_SEL 0x38

/*
 * This CPU's one-word percpu cell (holds a `struct processor *`, see
 * kern/i386/smp.h): a flat, 4-byte, read-write data segment based at that
 * word's own address. %fs stays loaded with this selector throughout kernel
 * execution on every CPU (i386_gdt_init(_secondary) load it; i386_trap_common
 * reloads it on every trap entry, since a ring-3 trap arrives with the user's
 * own %fs); reading %fs:0 is then machine_cpu_local() in one instruction.
 */
#define GDT_PERCPU_SEL 0x40

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

/*
 * Build CPU 0's GDT and both TSSes, reload every segment register and load
 * TR. Only the boot CPU calls this; a secondary calls i386_gdt_init_secondary
 * instead, once it knows its own logical id and boot stack.
 */
void i386_gdt_init(void);

/*
 * The same, for a secondary CPU: its own GDT, main and double-fault TSS and
 * percpu cell, at g_gdt[cpu_id] and friends. `boot_stack_top` is this CPU's
 * own kernel stack (both TSS.esp0's initial value and the double-fault
 * stack's top come from it -- a secondary does not get a second, separate
 * double-fault stack the way the boot CPU's own dedicated one does; a
 * smaller carve-out of the same boot stack is enough for how rarely a real
 * double fault is expected to fire during bring-up). Reads the double-fault
 * task's cr3 from pmap_kernel_directory_physical() itself: pmap_init() (the
 * only caller of i386_gdt_set_double_fault_cr3 today) has long since run by
 * the time any secondary starts.
 */
void i386_gdt_init_secondary(uint32_t cpu_id, uint32_t boot_stack_top);

/* Stack the *current* CPU switches to when a trap arrives from ring 3. */
void i386_gdt_set_kernel_stack(uint32_t esp0);

/*
 * Page directory the double-fault task runs on. The task switch loads CR3
 * from the TSS, so it must be a directory that maps the kernel (any address
 * space's kernel half will do, the master kernel directory is the safe pick).
 * Sets every CPU's double-fault TSS (there is, today, only ever the one
 * shared kernel directory to set it to); a secondary that starts after this
 * runs picks the same value up directly, see i386_gdt_init_secondary's
 * comment.
 */
void i386_gdt_set_double_fault_cr3(uint32_t cr3);

/*
 * The *current* CPU's main TSS. After a task switch to the double-fault TSS
 * the CPU stores the faulting context here, which is what the #DF report
 * reads.
 */
const i386_tss_t *i386_gdt_main_tss(void);

#endif
