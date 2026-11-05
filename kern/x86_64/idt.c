/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/idt.c
 *
 * Installs all 256 long-mode interrupt gates and assigns IST stacks to the
 * exceptions that must remain diagnosable after stack or processor faults.
 */

#include <kern/x86_64/gdt.h>
#include <kern/x86_64/trap.h>
#include <kern/machine/vm_param.h>

#include <kern/console/console.h>

#include <stddef.h>
#include <stdint.h>

#define X86_IDT_PRESENT (1U << 7U)
#define X86_IDT_DPL3 (3U << 5U)
#define X86_IDT_INTERRUPT_GATE 0x0EU

typedef struct __attribute__((packed)) {
	uint16_t offset_low;
	uint16_t selector;
	uint8_t ist;
	uint8_t attributes;
	uint16_t offset_middle;
	uint32_t offset_high;
	uint32_t reserved;
} x86_idt_gate_t;

typedef struct __attribute__((packed)) {
	uint16_t limit;
	uint64_t base;
} x86_idtr_t;

_Static_assert(sizeof(x86_idt_gate_t) == 16U, "x86 IDT gate size changed");

static x86_idt_gate_t g_x86_idt[X86_IDT_VECTOR_COUNT] __attribute__((aligned(16)));

/*
 * Routine:     x86_idt_set_gate
 * Purpose:
 *              Encode one 64-bit handler address, code selector, gate type,
 *              privilege level, and optional TSS interrupt-stack index.
 */
static void
x86_idt_set_gate(uint32_t vector, uint64_t handler, uint8_t ist, uint8_t attributes)
{
	x86_idt_gate_t *gate = &g_x86_idt[vector];

	gate->offset_low = (uint16_t)handler;
	gate->selector = X86_KERNEL_CODE_SELECTOR;
	gate->ist = ist & 0x07U;
	gate->attributes = attributes;
	gate->offset_middle = (uint16_t)(handler >> 16U);
	gate->offset_high = (uint32_t)(handler >> 32U);
	gate->reserved = 0U;
}

/*
 * Routine:     x86_idt_initialize
 * Purpose:
 *              Populate every IDT vector with a valid kernel gate, load IDTR,
 *              and enable safe exception entry after the GDT and TSS exist.
 */
bool
x86_idt_initialize(void)
{
	uint64_t table_base = (uint64_t)(uintptr_t)x86_isr_table + VMM_HIGHER_HALF_BASE;

	for (uint32_t vector = 0U; vector < X86_IDT_VECTOR_COUNT; vector++) {
		uint64_t handler = table_base + (uint64_t)x86_isr_table[vector];
		uint8_t ist = 0U;
		uint8_t attributes = X86_IDT_PRESENT | X86_IDT_INTERRUPT_GATE;

		if (vector == X86_TRAP_DOUBLE_FAULT) {
			ist = 1U;
		} else if (vector == 2U) {
			ist = 2U;
		} else if (vector == X86_TRAP_MACHINE_CHECK) {
			ist = 3U;
		}

		if (vector == X86_TRAP_BREAKPOINT || vector == X86_TRAP_OVERFLOW) {
			attributes = X86_IDT_PRESENT | X86_IDT_DPL3 | X86_IDT_INTERRUPT_GATE;
		}

		x86_idt_set_gate(vector, handler, ist, attributes);
	}

	x86_idtr_t descriptor = {
		.limit = sizeof(g_x86_idt) - 1U,
		.base = (uint64_t)(uintptr_t)g_x86_idt + VMM_HIGHER_HALF_BASE,
	};
	__asm__ volatile("lidt %0" : : "m"(descriptor) : "memory");

	kprintf("x86_idt_initialize: installed %u interrupt gates\n", X86_IDT_VECTOR_COUNT);
	kputln("x86_idt_initialize: vectors 0-31 route to exception diagnostics");
	kputln("x86_idt_initialize: vectors 32-255 route to unexpected-interrupt handling");
	kputln("x86_idt_initialize: breakpoint and overflow gates are callable from ring 3");
	kputln("x86_idt_initialize: double fault, NMI, and machine check use IST stacks");

	return true;
}
