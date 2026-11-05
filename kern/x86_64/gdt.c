/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/gdt.c
 *
 * Installs the kernel long-mode segments and the TSS interrupt stacks.
 */

#include <kern/x86_64/gdt.h>
#include <kern/machine/vm_param.h>

#include <kern/console/console.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define X86_TSS_IST_STACK_SIZE (16U * 1024U)
#define X86_TSS_IST_COUNT 3U

typedef struct __attribute__((packed)) {
	uint32_t reserved0;
	uint64_t rsp[3];
	uint64_t reserved1;
	uint64_t ist[7];
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t io_map_base;
} x86_tss_t;

typedef struct __attribute__((packed)) {
	uint16_t limit;
	uint64_t base;
} x86_descriptor_pointer_t;

static uint64_t g_x86_gdt[7] __attribute__((aligned(16)));
static x86_tss_t g_x86_tss __attribute__((aligned(16)));
static uint8_t g_x86_ist_stacks[X86_TSS_IST_COUNT][X86_TSS_IST_STACK_SIZE] __attribute__((aligned(16)));

extern void x86_gdt_load(const x86_descriptor_pointer_t *descriptor);

/*
 * Routine:     x86_gdt_initialize
 * Purpose:
 *              Replace firmware segmentation state and install dedicated
 *              double-fault, NMI, and machine-check interrupt stacks.
 */
bool
x86_gdt_initialize(uint64_t kernel_stack_top)
{
	if (kernel_stack_top == 0ULL) {
		return false;
	}

	memset(g_x86_gdt, 0, sizeof(g_x86_gdt));
	memset(&g_x86_tss, 0, sizeof(g_x86_tss));
	memset(g_x86_ist_stacks, 0, sizeof(g_x86_ist_stacks));

	g_x86_gdt[1] = 0x00AF9A000000FFFFULL;
	g_x86_gdt[2] = 0x00CF92000000FFFFULL;
	g_x86_gdt[3] = 0x00CFF2000000FFFFULL;
	g_x86_gdt[4] = 0x00AFFA000000FFFFULL;

	g_x86_tss.rsp[0] = kernel_stack_top;
	for (uint32_t index = 0U; index < X86_TSS_IST_COUNT; index++) {
		g_x86_tss.ist[index] = (uint64_t)(uintptr_t)&g_x86_ist_stacks[index][X86_TSS_IST_STACK_SIZE] +
			VMM_HIGHER_HALF_BASE;
	}
	g_x86_tss.io_map_base = sizeof(g_x86_tss);

	uint64_t tss_base = (uint64_t)(uintptr_t)&g_x86_tss + VMM_HIGHER_HALF_BASE;
	uint64_t tss_limit = sizeof(g_x86_tss) - 1U;
	g_x86_gdt[5] = (tss_limit & 0xFFFFULL) | ((tss_base & 0xFFFFFFULL) << 16U) | (0x89ULL << 40U) |
		((tss_limit & 0xF0000ULL) << 32U) | ((tss_base & 0xFF000000ULL) << 32U);
	g_x86_gdt[6] = tss_base >> 32U;

	x86_descriptor_pointer_t descriptor = {
		.limit = sizeof(g_x86_gdt) - 1U,
		.base = (uint64_t)(uintptr_t)g_x86_gdt + VMM_HIGHER_HALF_BASE,
	};
	x86_gdt_load(&descriptor);

	kprintf("x86_gdt_initialize: kernel selectors installed\n");
	kprintf("x86_gdt_initialize: TSS at 0x%llx, RSP0 0x%llx\n", (unsigned long long)tss_base,
			(unsigned long long)kernel_stack_top);
	kprintf("x86_gdt_initialize: IST stacks ready for double fault, NMI, and machine check\n");

	return true;
}
