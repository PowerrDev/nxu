/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/boot.c
 *
 * First C-stage boot entry for the native x86-64 kernel.
 */

#include <boot/handoff.h>
#include <kern/console/console.h>
#include <kern/logging/version.h>
#include <kern/machine/vm_param.h>
#include <kern/x86_64/gdt.h>
#include <kern/x86_64/smp.h>
#include <kern/x86_64/trap.h>
#include <platform/x86_64/uart.h>
#include <vm/x86_64/bootstrap.h>

#include <stdint.h>

typedef void (*x86_kernel_entry_t)(const struct nxu_boot_x86_64 *boot) __attribute__((noreturn));

#define X86_CPUID_NX (1U << 20U)
#define X86_CPUID_LONG_MODE (1U << 29U)

/*
 * Routine:     x86_log_processor
 * Purpose:
 *              Report the boot processor identity, key long-mode features,
 *              and paging control state before replacing firmware tables.
 */
static void
x86_log_processor(void)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint32_t maximum_leaf;
	uint32_t maximum_extended_leaf;
	bool tsc_available = false;
	bool apic_available = false;
	char vendor[13];
	uintptr_t cr0;
	uintptr_t cr3;
	uintptr_t cr4;

	eax = 0U;
	__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
	maximum_leaf = eax;
	for (uint32_t index = 0U; index < 4U; index++) {
		vendor[index] = (char)(ebx >> (index * 8U));
		vendor[index + 4U] = (char)(edx >> (index * 8U));
		vendor[index + 8U] = (char)(ecx >> (index * 8U));
	}
	vendor[12] = '\0';

	eax = 0x80000000U;
	__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
	maximum_extended_leaf = eax;

	cr0 = 0UL;
	cr3 = 0UL;
	cr4 = 0UL;
	__asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
	__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
	__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

	kprintf("x86_log_processor: build %s, clang %u.%u.%u\n", NXU_BUILD, __clang_major__, __clang_minor__,
			__clang_patchlevel__);
	kconsole_set_timestamp(false);
	kprintf("x86_log_processor: vendor %s, initial APIC ID %llu\n", vendor,
			(unsigned long long)machine_cpu_mpidr());

	if (maximum_leaf >= 1U) {
		eax = 1U;
		__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
		tsc_available = (edx & (1U << 4U)) != 0U;
		apic_available = (edx & (1U << 9U)) != 0U;
		uint32_t family = (eax >> 8U) & 0xFU;
		uint32_t model = (eax >> 4U) & 0xFU;
		if (family == 0xFU) family += (eax >> 20U) & 0xFFU;
		if (family == 0x6U || family == 0xFU) model |= ((eax >> 16U) & 0xFU) << 4U;
		kprintf("x86_log_processor: family 0x%x, model 0x%x, stepping %u\n", family, model, eax & 0xFU);
		kprintf("x86_log_processor: local APIC %s\n",
				apic_available ? "available" : "unavailable");
	}

	if (maximum_extended_leaf >= 0x80000001U) {
		eax = 0x80000001U;
		__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
		kprintf("x86_log_processor: long mode %s\n", (edx & X86_CPUID_LONG_MODE) != 0U ? "available" : "unavailable");
		kprintf("x86_log_processor: NX %s\n", (edx & X86_CPUID_NX) != 0U ? "available" : "unavailable");
		if (maximum_extended_leaf >= 0x80000008U) {
			eax = 0x80000008U;
			__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
			kprintf("x86_log_processor: physical address width %u bits\n", eax & 0xFFU);
		} else {
			kputln("x86_log_processor: physical address width unknown");
		}
	}

	kprintf("x86_log_processor: CR0 0x%llx\n", (unsigned long long)cr0);
	kprintf("x86_log_processor: CR3 0x%llx\n", (unsigned long long)cr3);
	kprintf("x86_log_processor: CR4 0x%llx\n", (unsigned long long)cr4);
	kprintf("x86_log_processor: TSC %s\n", tsc_available ? "available" : "not detected");
	kputln("x86_log_processor: logical processor count 1");
}

extern void x86_enter_higher_half(x86_kernel_entry_t entry, const struct nxu_boot_x86_64 *boot,
								  uintptr_t stack_top) __attribute__((noreturn));

/*
 * Routine:     x86_kernel_high_main
 * Purpose:
 *              Continue boot after execution switches to the kernel's
 *              higher-half direct-map alias.
 */
void x86_kernel_high_main(const struct nxu_boot_x86_64 *boot);

/*
 * Routine:     x86_kernel_main
 * Purpose:
 *              Validate the loader handoff, bring up serial output, and prove
 *              the 64-bit kernel entry is running before VM and drivers exist.
 */
void
x86_kernel_main(const struct nxu_boot_x86_64 *boot)
{
	x86_uart_initialize();
	kconsole_set_verbose(true);
	kconsole_set_timestamp(true);
	kputln("x86_kernel_main: kernel entry reached");
	kconsole_set_timestamp(false);

	if (boot == 0 || boot->magic != NXU_BOOT_X86_64_MAGIC || boot->version != NXU_BOOT_X86_64_VERSION ||
		boot->size != sizeof(*boot)) {
		kputln("x86_kernel_main: invalid UEFI handoff");
		return;
	}

	kconsole_set_timestamp(true);
	kprintf("x86_kernel_main: %s\n", NXU_KERNEL_VERSION);
	x86_log_processor();
	kprintf("x86_kernel_main: handoff 0x%llx, size %u, version %u\n", (unsigned long long)(uintptr_t)boot,
			boot->size, boot->version);
	kprintf("x86_kernel_main: RSDP 0x%llx\n", (unsigned long long)boot->rsdp);
	kprintf("x86_kernel_main: boot arguments 0x%llx\n", (unsigned long long)boot->bootargs);
	kprintf("x86_kernel_main: stack top 0x%llx\n", (unsigned long long)boot->stack_top);
	kprintf("x86_kernel_main: UEFI map address 0x%llx\n", (unsigned long long)boot->memory_map);
	kprintf("x86_kernel_main: UEFI map size %llu bytes\n", (unsigned long long)boot->memory_map_size);
	kprintf("x86_kernel_main: UEFI descriptor stride %llu bytes\n",
			(unsigned long long)boot->memory_descriptor_size);

	if (!x86_vm_bootstrap(boot)) {
		kputln("x86_kernel_main: page-table bootstrap failed");
		return;
	}

	kputln("x86_kernel_main: NXU page tables active");
	uintptr_t active_cr3;
	__asm__ volatile("mov %%cr3, %0" : "=r"(active_cr3));
	kprintf("x86_vm_bootstrap: root 0x%llx\n", (unsigned long long)active_cr3);
	kputln("x86_vm_bootstrap: four-level paging active");
	kputln("x86_vm_bootstrap: identity map covers 0-1024 MiB");
	kputln("x86_vm_bootstrap: direct map covers 0-1024 MiB");
	kputln("x86_kernel_main: initializing physical and virtual memory");

	if (!x86_vm_initialize(boot)) {
		kputln("x86_kernel_main: VM initialization failed");
		return;
	}

	kputln("x86_kernel_main: VM initialization complete");
	kputln("x86_kernel_main: switching to higher-half aliases");

	uintptr_t high_entry = (uintptr_t)x86_kernel_high_main;
	uintptr_t high_boot = (uintptr_t)boot;
	uintptr_t high_stack = (uintptr_t)boot->stack_top;

	high_entry += (uintptr_t)VMM_HIGHER_HALF_BASE;
	high_boot += (uintptr_t)VMM_HIGHER_HALF_BASE;
	high_stack += (uintptr_t)VMM_HIGHER_HALF_BASE;

	x86_enter_higher_half((x86_kernel_entry_t)high_entry, (const struct nxu_boot_x86_64 *)high_boot, high_stack);
}

/*
 * Routine:     x86_kernel_high_main
 * Purpose:
 *              Confirm the kernel continues to execute with its code, boot
 *              handoff, and stack through the permanent direct-map alias.
 */
void
x86_kernel_high_main(const struct nxu_boot_x86_64 *boot)
{
	if (boot == 0 || boot->magic != NXU_BOOT_X86_64_MAGIC || boot->version != NXU_BOOT_X86_64_VERSION) {
		kputln("x86_kernel_high_main: higher-half handoff is invalid");
	} else {
		uint64_t stack_top = boot->stack_top + VMM_HIGHER_HALF_BASE;
		if (!x86_gdt_initialize(stack_top)) {
			kputln("x86_kernel_high_main: GDT and TSS initialization failed");
			for (;;) {
				__asm__ volatile("cli; hlt" : : : "memory");
			}
		}

		if (!x86_idt_initialize()) {
			kputln("x86_kernel_high_main: IDT initialization failed");
			for (;;) {
				__asm__ volatile("cli; hlt" : : : "memory");
			}
		}

		if (!x86_trap_self_test()) {
			kputln("x86_kernel_high_main: IDT exception checks failed");
			for (;;) {
				__asm__ volatile("cli; hlt" : : : "memory");
			}
		}
		kputln("x86_kernel_high_main: breakpoint and page-fault paths passed");

		kputln("x86_kernel_high_main: higher-half kernel alias active");
		kprintf("x86_kernel_high_main: code address 0x%llx\n",
				(unsigned long long)((uintptr_t)x86_kernel_high_main + VMM_HIGHER_HALF_BASE));
		kprintf("x86_kernel_high_main: handoff address 0x%llx\n", (unsigned long long)(uintptr_t)boot);
		kprintf("x86_kernel_high_main: stack top 0x%llx\n", (unsigned long long)boot->stack_top + VMM_HIGHER_HALF_BASE);
		kputln("x86_kernel_high_main: interrupts remain disabled until a controller is initialized");
	}

	for (;;) {
		__asm__ volatile("cli; hlt" : : : "memory");
	}
}
