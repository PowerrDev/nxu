/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/idt.c
 *
 * See idt.h.
 */

#include <kern/i386/idt.h>

#include <kern/i386/gdt.h>
#include <kern/i386/trap.h>

#include <kern/console/console.h>

#include <stdint.h>

typedef struct __attribute__((packed)) {
	uint16_t offset_low;
	uint16_t selector;
	uint8_t reserved;
	uint8_t attributes;
	uint16_t offset_high;
} idt_entry_t;

typedef struct __attribute__((packed)) {
	uint16_t limit;
	uint32_t base;
} idt_pointer_t;

_Static_assert(sizeof(idt_entry_t) == 8U, "idt_entry_t must be 8 bytes");

static idt_entry_t g_idt[IDT_ENTRY_COUNT];
static idt_pointer_t g_idt_pointer;

void i386_idt_load_secondary(void)
{
	__asm__ volatile("lidt %0" : : "m"(g_idt_pointer) : "memory");
}

void i386_idt_set_gate(uint8_t vector, uint32_t offset, uint16_t selector, uint8_t attributes)
{
	g_idt[vector].offset_low = (uint16_t)(offset & 0xFFFFU);
	g_idt[vector].selector = selector;
	g_idt[vector].reserved = 0U;
	g_idt[vector].attributes = attributes;
	g_idt[vector].offset_high = (uint16_t)((offset >> 16U) & 0xFFFFU);
}

void i386_idt_init(void)
{
	for (uint32_t vector = 0U; vector < IDT_ENTRY_COUNT; vector++) {
		g_idt[vector] = (idt_entry_t){ 0 };
	}

	for (uint32_t vector = 0U; vector < I386_ISR_COUNT; vector++) {
		i386_idt_set_gate(
			(uint8_t)vector,
			i386_isr_table[vector],
			GDT_KERNEL_CODE_SEL,
			IDT_GATE_INTERRUPT
		);
	}

	/*
	 * #DF is delivered through a task gate: the CPU switches to the
	 * dedicated double-fault TSS and its private stack, so the report still
	 * comes out if the fault was caused by the kernel stack itself.
	 */
	i386_idt_set_gate(T_DOUBLE_FAULT, 0U, GDT_DF_TSS_SEL, IDT_GATE_TASK);

	i386_idt_set_gate(
		T_SYSCALL,
		(uint32_t)(uintptr_t)i386_isr_syscall,
		GDT_KERNEL_CODE_SEL,
		IDT_GATE_INTERRUPT_USER
	);

	/*
	 * IPIs and the Local APIC's spurious vector (see kern/i386/apic.c,
	 * smp.c): kernel-only, ring 0 delivery like every hardware IRQ gate.
	 */
	i386_idt_set_gate(T_IPI_RESCHEDULE, (uint32_t)(uintptr_t)i386_isr_ipi_reschedule, GDT_KERNEL_CODE_SEL, IDT_GATE_INTERRUPT);
	i386_idt_set_gate(T_IPI_CPU_STOP, (uint32_t)(uintptr_t)i386_isr_ipi_cpu_stop, GDT_KERNEL_CODE_SEL, IDT_GATE_INTERRUPT);
	i386_idt_set_gate(T_IPI_TICK, (uint32_t)(uintptr_t)i386_isr_ipi_tick, GDT_KERNEL_CODE_SEL, IDT_GATE_INTERRUPT);
	i386_idt_set_gate(T_LAPIC_SPURIOUS, (uint32_t)(uintptr_t)i386_isr_lapic_spurious, GDT_KERNEL_CODE_SEL, IDT_GATE_INTERRUPT);

	g_idt_pointer.limit = (uint16_t)(sizeof(g_idt) - 1U);
	g_idt_pointer.base = (uint32_t)(uintptr_t)g_idt;

	__asm__ volatile("lidt %0" : : "m"(g_idt_pointer) : "memory");

	kprintf(
		"i386_idt_init: %u gates at %p, exceptions 0-%u, irqs %u-%u, syscall 0x%x, ipi 0x%x-0x%x, spurious 0x%x\n",
		IDT_ENTRY_COUNT,
		(void *)g_idt,
		T_EXCEPTION_COUNT - 1U,
		T_IRQ_BASE,
		T_IRQ_BASE + T_IRQ_COUNT - 1U,
		T_SYSCALL,
		T_IPI_RESCHEDULE,
		T_IPI_CPU_STOP,
		T_LAPIC_SPURIOUS
	);
}
