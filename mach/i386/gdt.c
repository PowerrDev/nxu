/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/gdt.c
 *
 * See gdt.h.
 */

#include <mach/i386/gdt.h>

#include <mach/i386/trap.h>

#include <kern/console/console.h>

#include <stdint.h>

#define STR_(value) #value
#define STR(value) STR_(value)

#define GDT_ENTRY_COUNT 8U

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
/* Byte granularity, used by the TSS descriptors. */
#define GDT_FLAGS_BYTE 0x0U

#define DOUBLE_FAULT_STACK_SIZE 4096U

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

static gdt_entry_t g_gdt[GDT_ENTRY_COUNT];
static gdt_pointer_t g_gdt_pointer;

static i386_tss_t g_main_tss;
static i386_tss_t g_double_fault_tss;

static uint8_t g_double_fault_stack[DOUBLE_FAULT_STACK_SIZE] __attribute__((aligned(16)));

extern uint8_t i386_boot_stack_top[];

static void gdt_set_entry(
	uint32_t index,
	uint32_t base,
	uint32_t limit,
	uint8_t access,
	uint8_t flags
)
{
	g_gdt[index].limit_low = (uint16_t)(limit & 0xFFFFU);
	g_gdt[index].base_low = (uint16_t)(base & 0xFFFFU);
	g_gdt[index].base_middle = (uint8_t)((base >> 16U) & 0xFFU);
	g_gdt[index].access = access;
	g_gdt[index].limit_high_flags = (uint8_t)(((limit >> 16U) & 0x0FU) | ((uint32_t)flags << 4U));
	g_gdt[index].base_high = (uint8_t)((base >> 24U) & 0xFFU);
}

static void gdt_set_tss(uint32_t index, const i386_tss_t *tss)
{
	gdt_set_entry(
		index,
		(uint32_t)(uintptr_t)tss,
		(uint32_t)sizeof(i386_tss_t) - 1U,
		GDT_ACCESS_TSS,
		GDT_FLAGS_BYTE
	);
}

void i386_gdt_init(void)
{
	/* Slot 0 is the mandatory null descriptor. */
	gdt_set_entry(1U, 0U, 0xFFFFFU, GDT_ACCESS_KERNEL_CODE, GDT_FLAGS_FLAT);
	gdt_set_entry(2U, 0U, 0xFFFFFU, GDT_ACCESS_KERNEL_DATA, GDT_FLAGS_FLAT);
	gdt_set_entry(3U, 0U, 0xFFFFFU, GDT_ACCESS_USER_CODE, GDT_FLAGS_FLAT);
	gdt_set_entry(4U, 0U, 0xFFFFFU, GDT_ACCESS_USER_DATA, GDT_FLAGS_FLAT);

	/*
	 * Slot 7 is a well-formed data descriptor with the present bit clear,
	 * so loading it raises #NP. (An all-zero slot would raise #GP instead:
	 * the CPU checks the descriptor type before the present bit.)
	 */
	gdt_set_entry(7U, 0U, 0xFFFFFU, GDT_ACCESS_NOT_PRESENT, GDT_FLAGS_FLAT);

	g_main_tss.ss0 = GDT_KERNEL_DATA_SEL;
	g_main_tss.esp0 = (uint32_t)(uintptr_t)i386_boot_stack_top;
	/* No I/O permission bitmap: an offset past the limit denies all ports to ring 3. */
	g_main_tss.iomap_base = (uint16_t)sizeof(i386_tss_t);

	/*
	 * The double-fault task starts fresh on its own stack every time, so
	 * only the fields the CPU loads on the task switch are meaningful.
	 * cr3 stays 0 until paging exists; the VM layer must copy the live
	 * page directory here when it turns paging on.
	 */
	g_double_fault_tss.ss0 = GDT_KERNEL_DATA_SEL;
	g_double_fault_tss.esp0 = (uint32_t)(uintptr_t)&g_double_fault_stack[DOUBLE_FAULT_STACK_SIZE];
	g_double_fault_tss.eip = (uint32_t)(uintptr_t)i386_double_fault_task;
	g_double_fault_tss.eflags = 0x2U;
	g_double_fault_tss.esp = (uint32_t)(uintptr_t)&g_double_fault_stack[DOUBLE_FAULT_STACK_SIZE];
	g_double_fault_tss.cs = GDT_KERNEL_CODE_SEL;
	g_double_fault_tss.ss = GDT_KERNEL_DATA_SEL;
	g_double_fault_tss.ds = GDT_KERNEL_DATA_SEL;
	g_double_fault_tss.es = GDT_KERNEL_DATA_SEL;
	g_double_fault_tss.fs = GDT_KERNEL_DATA_SEL;
	g_double_fault_tss.gs = GDT_KERNEL_DATA_SEL;
	g_double_fault_tss.iomap_base = (uint16_t)sizeof(i386_tss_t);

	gdt_set_tss(5U, &g_main_tss);
	gdt_set_tss(6U, &g_double_fault_tss);

	g_gdt_pointer.limit = (uint16_t)(sizeof(g_gdt) - 1U);
	g_gdt_pointer.base = (uint32_t)(uintptr_t)g_gdt;

	__asm__ volatile(
		"lgdt %0\n\t"
		"ljmp $" STR(GDT_KERNEL_CODE_SEL) ", $1f\n"
		"1:\n\t"
		"movw $" STR(GDT_KERNEL_DATA_SEL) ", %%ax\n\t"
		"movw %%ax, %%ds\n\t"
		"movw %%ax, %%es\n\t"
		"movw %%ax, %%fs\n\t"
		"movw %%ax, %%gs\n\t"
		"movw %%ax, %%ss\n\t"
		"movw $" STR(GDT_TSS_SEL) ", %%ax\n\t"
		"ltr %%ax"
		:
		: "m"(g_gdt_pointer)
		: "eax", "memory"
	);

	kprintf(
		"i386_gdt_init: %u descriptors at %p, main tss %p, double-fault tss %p\n",
		GDT_ENTRY_COUNT,
		(void *)g_gdt,
		(void *)&g_main_tss,
		(void *)&g_double_fault_tss
	);
}

void i386_gdt_set_kernel_stack(uint32_t esp0)
{
	g_main_tss.esp0 = esp0;
}

const i386_tss_t *i386_gdt_main_tss(void)
{
	return &g_main_tss;
}
