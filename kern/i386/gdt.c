/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/gdt.c
 *
 * See gdt.h.
 */

#include <kern/i386/gdt.h>

#include <kern/i386/pmap.h>
#include <kern/i386/smp.h>
#include <kern/i386/trap.h>

#include <kern/console/console.h>
#include <kern/cpuset.h>

#include <stdint.h>

#define STR_(value) #value
#define STR(value) STR_(value)

#define GDT_ENTRY_COUNT 9U

/* Access byte: present, DPL, descriptor type, executable, RW / readable. */
#define GDT_ACCESS_KERNEL_CODE 0x9AU
#define GDT_ACCESS_KERNEL_DATA 0x92U
#define GDT_ACCESS_USER_CODE 0xFAU
#define GDT_ACCESS_USER_DATA 0xF2U
/* Data segment, read/write, but with the present bit clear. */
#define GDT_ACCESS_NOT_PRESENT 0x12U
/* System descriptor: present, DPL 0, 32-bit TSS (available). */
#define GDT_ACCESS_TSS 0x89U

/* Flags nibble: 4 KiB granularity, 32-bit segment. */
#define GDT_FLAGS_FLAT 0xCU
/* Byte granularity, used by the TSS and percpu descriptors: both are sized in bytes, not pages. */
#define GDT_FLAGS_BYTE 0x0U

/*
 * A secondary's double-fault stack: a slice of its own boot stack, not a
 * separate carve-out the way the boot CPU's g_boot_double_fault_stack is
 * (see i386_gdt_init_secondary's doc comment).
 */
#define SECONDARY_DF_STACK_SIZE 1024U

#define BOOT_DOUBLE_FAULT_STACK_SIZE 4096U

typedef struct __attribute__((packed)) {
	uint16_t limit_low;
	uint16_t base_low;
	uint8_t base_middle;
	uint8_t access;
	uint8_t limit_high_flags;
	uint8_t base_high;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
	uint16_t limit;
	uint32_t base;
} gdt_pointer_t;

_Static_assert(sizeof(gdt_entry_t) == 8U, "gdt_entry_t must be 8 bytes");

/* One full table, one main TSS, one double-fault TSS and one percpu cell per CPU; see the file comment in gdt.h. */
static gdt_entry_t g_gdt[NXU_MAX_CPUS][GDT_ENTRY_COUNT];
static gdt_pointer_t g_gdt_pointer[NXU_MAX_CPUS];
static i386_tss_t g_main_tss[NXU_MAX_CPUS];
static i386_tss_t g_double_fault_tss[NXU_MAX_CPUS];
static void *g_percpu_cell[NXU_MAX_CPUS];

static uint8_t g_boot_double_fault_stack[BOOT_DOUBLE_FAULT_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t g_secondary_double_fault_stack[NXU_MAX_CPUS][SECONDARY_DF_STACK_SIZE] __attribute__((aligned(16)));

extern uint8_t i386_boot_stack_top[];

static void gdt_set_entry(
	uint32_t cpu_id,
	uint32_t index,
	uint32_t base,
	uint32_t limit,
	uint8_t access,
	uint8_t flags
)
{
	g_gdt[cpu_id][index].limit_low = (uint16_t)(limit & 0xFFFFU);
	g_gdt[cpu_id][index].base_low = (uint16_t)(base & 0xFFFFU);
	g_gdt[cpu_id][index].base_middle = (uint8_t)((base >> 16U) & 0xFFU);
	g_gdt[cpu_id][index].access = access;
	g_gdt[cpu_id][index].limit_high_flags = (uint8_t)(((limit >> 16U) & 0x0FU) | ((uint32_t)flags << 4U));
	g_gdt[cpu_id][index].base_high = (uint8_t)((base >> 24U) & 0xFFU);
}

static void gdt_set_tss(uint32_t cpu_id, uint32_t index, const i386_tss_t *tss)
{
	gdt_set_entry(
		cpu_id,
		index,
		(uint32_t)(uintptr_t)tss,
		(uint32_t)sizeof(i386_tss_t) - 1U,
		GDT_ACCESS_TSS,
		GDT_FLAGS_BYTE
	);
}

/*
 * Everything one CPU's GDT, TSSes and percpu cell need that does not depend
 * on whether this is the boot CPU or a secondary: the five flat descriptors,
 * the not-present spare, the percpu segment, and the double-fault TSS's
 * fixed fields (cr3 comes from pmap separately since it is not known yet the
 * first time the boot CPU calls this). Does not touch any control register
 * or segment register; the caller does that once both TSSes are ready to be
 * pointed at.
 */
static void gdt_build_common(uint32_t cpu_id, const i386_tss_t *main_tss, i386_tss_t *df_tss, uint32_t df_stack_top, uint32_t df_entry)
{
	gdt_set_entry(cpu_id, 1U, 0U, 0xFFFFFU, GDT_ACCESS_KERNEL_CODE, GDT_FLAGS_FLAT);
	gdt_set_entry(cpu_id, 2U, 0U, 0xFFFFFU, GDT_ACCESS_KERNEL_DATA, GDT_FLAGS_FLAT);
	gdt_set_entry(cpu_id, 3U, 0U, 0xFFFFFU, GDT_ACCESS_USER_CODE, GDT_FLAGS_FLAT);
	gdt_set_entry(cpu_id, 4U, 0U, 0xFFFFFU, GDT_ACCESS_USER_DATA, GDT_FLAGS_FLAT);

	/*
	 * Slot 7 is a well-formed data descriptor with the present bit clear,
	 * so loading it raises #NP. (An all-zero slot would raise #GP instead:
	 * the CPU checks the descriptor type before the present bit.)
	 */
	gdt_set_entry(cpu_id, 7U, 0U, 0xFFFFFU, GDT_ACCESS_NOT_PRESENT, GDT_FLAGS_FLAT);

	/* Slot 8: this CPU's one-word percpu cell, see GDT_PERCPU_SEL's doc comment. */
	gdt_set_entry(cpu_id, 8U, (uint32_t)(uintptr_t)&g_percpu_cell[cpu_id], (uint32_t)sizeof(void *) - 1U, GDT_ACCESS_KERNEL_DATA, GDT_FLAGS_BYTE);

	g_percpu_cell[cpu_id] = 0;

	df_tss->ss0 = GDT_KERNEL_DATA_SEL;
	df_tss->esp0 = df_stack_top;
	/* No I/O permission bitmap: an offset past the limit denies all ports to ring 3. */
	df_tss->iomap_base = (uint16_t)sizeof(i386_tss_t);

	df_tss->eflags = 0x2U;
	df_tss->esp = df_stack_top;
	df_tss->eip = df_entry;
	df_tss->cs = GDT_KERNEL_CODE_SEL;
	df_tss->ss = GDT_KERNEL_DATA_SEL;
	df_tss->ds = GDT_KERNEL_DATA_SEL;
	df_tss->es = GDT_KERNEL_DATA_SEL;
	df_tss->fs = GDT_PERCPU_SEL;
	df_tss->gs = GDT_KERNEL_DATA_SEL;
	df_tss->iomap_base = (uint16_t)sizeof(i386_tss_t);

	gdt_set_tss(cpu_id, 5U, main_tss);
	gdt_set_tss(cpu_id, 6U, df_tss);

	g_gdt_pointer[cpu_id].limit = (uint16_t)(sizeof(g_gdt[cpu_id]) - 1U);
	g_gdt_pointer[cpu_id].base = (uint32_t)(uintptr_t)g_gdt[cpu_id];
}

void i386_gdt_init(void)
{
	g_main_tss[0].ss0 = GDT_KERNEL_DATA_SEL;
	g_main_tss[0].esp0 = (uint32_t)(uintptr_t)i386_boot_stack_top;
	g_main_tss[0].iomap_base = (uint16_t)sizeof(i386_tss_t);

	/*
	 * The double-fault task starts fresh on its own stack every time, so
	 * only the fields the CPU loads on the task switch are meaningful.
	 * start.S turns paging on before any C code runs, so cr3 starts as the
	 * boot page directory (the master kernel directory the VM layer keeps
	 * refining in place); i386_gdt_set_double_fault_cr3() lets the VM layer
	 * republish it should the master directory ever move.
	 */
	{
		uint32_t cr3;

		__asm__ volatile("movl %%cr3, %0" : "=r"(cr3));
		g_double_fault_tss[0].cr3 = cr3;
	}

	gdt_build_common(
		0U,
		&g_main_tss[0],
		&g_double_fault_tss[0],
		(uint32_t)(uintptr_t)&g_boot_double_fault_stack[BOOT_DOUBLE_FAULT_STACK_SIZE],
		(uint32_t)(uintptr_t)i386_double_fault_task
	);

	__asm__ volatile(
		"lgdt %0\n\t"
		"ljmp $" STR(GDT_KERNEL_CODE_SEL) ", $1f\n"
		"1:\n\t"
		"movw $" STR(GDT_KERNEL_DATA_SEL) ", %%ax\n\t"
		"movw %%ax, %%ds\n\t"
		"movw %%ax, %%es\n\t"
		"movw %%ax, %%gs\n\t"
		"movw %%ax, %%ss\n\t"
		"movw $" STR(GDT_PERCPU_SEL) ", %%ax\n\t"
		"movw %%ax, %%fs\n\t"
		"movw $" STR(GDT_TSS_SEL) ", %%ax\n\t"
		"ltr %%ax"
		:
		: "m"(g_gdt_pointer[0])
		: "eax", "memory"
	);

	kprintf(
		"i386_gdt_init: found %u descriptors at address %p.\n",
		GDT_ENTRY_COUNT,
		(void *)g_gdt[0]
	);

	kprintf("i386_gdt_init: main tss at address %p\n", (void *)&g_main_tss[0]);
	kprintf("i386_gdt_init: double fault tss %p\n", (void *)&g_double_fault_tss[0]);
}

void i386_gdt_init_secondary(uint32_t cpu_id, uint32_t boot_stack_top)
{
	if (cpu_id == 0U || cpu_id >= NXU_MAX_CPUS) return;

	g_main_tss[cpu_id].ss0 = GDT_KERNEL_DATA_SEL;
	g_main_tss[cpu_id].esp0 = boot_stack_top;
	g_main_tss[cpu_id].iomap_base = (uint16_t)sizeof(i386_tss_t);

	g_double_fault_tss[cpu_id].cr3 = pmap_kernel_directory_physical();

	gdt_build_common(
		cpu_id,
		&g_main_tss[cpu_id],
		&g_double_fault_tss[cpu_id],
		(uint32_t)(uintptr_t)&g_secondary_double_fault_stack[cpu_id][SECONDARY_DF_STACK_SIZE],
		(uint32_t)(uintptr_t)i386_double_fault_task
	);

	__asm__ volatile(
		"lgdt %0\n\t"
		"ljmp $" STR(GDT_KERNEL_CODE_SEL) ", $1f\n"
		"1:\n\t"
		"movw $" STR(GDT_KERNEL_DATA_SEL) ", %%ax\n\t"
		"movw %%ax, %%ds\n\t"
		"movw %%ax, %%es\n\t"
		"movw %%ax, %%gs\n\t"
		"movw %%ax, %%ss\n\t"
		"movw $" STR(GDT_PERCPU_SEL) ", %%ax\n\t"
		"movw %%ax, %%fs\n\t"
		"movw $" STR(GDT_TSS_SEL) ", %%ax\n\t"
		"ltr %%ax"
		:
		: "m"(g_gdt_pointer[cpu_id])
		: "eax", "memory"
	);
}

void i386_gdt_set_kernel_stack(uint32_t esp0)
{
	g_main_tss[machine_cpu_id()].esp0 = esp0;
}

void i386_gdt_set_double_fault_cr3(uint32_t cr3)
{
	for (uint32_t cpu_id = 0U; cpu_id < NXU_MAX_CPUS; cpu_id++) g_double_fault_tss[cpu_id].cr3 = cr3;
}

const i386_tss_t *i386_gdt_main_tss(void)
{
	return &g_main_tss[machine_cpu_id()];
}
