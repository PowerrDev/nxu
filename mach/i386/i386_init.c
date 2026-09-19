/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/i386_init.c
 *
 * First C code on an x86 boot. Brings up the serial console and the
 * counter, reports what the bootloader and CPU told us, and stops. The rest
 * of kern_init (memory management, scheduler, userland) is not ported to
 * this architecture yet; this is the seam it will be entered from.
 */

#include <mach/i386/gdt.h>
#include <mach/i386/idt.h>
#include <mach/i386/io.h>
#include <mach/i386/multiboot.h>
#include <mach/i386/timer.h>
#include <mach/i386/trap.h>

#include <kern/console/console.h>
#include <kern/logging/version.h>

#include <stdbool.h>
#include <stdint.h>

#define I386_CMDLINE_VALUE_MAX 32U

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

static void i386_cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
	__asm__ volatile(
		"cpuid"
		: "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
		: "a"(leaf), "c"(0U)
	);
}

static void i386_print_cpu(void)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	char vendor[13];

	i386_cpuid(0U, &eax, &ebx, &ecx, &edx);

	for (uint32_t index = 0U; index < 4U; index++) {
		vendor[index] = (char)(ebx >> (index * 8U));
		vendor[index + 4U] = (char)(edx >> (index * 8U));
		vendor[index + 8U] = (char)(ecx >> (index * 8U));
	}
	vendor[12] = '\0';

	uint32_t max_leaf = eax;

	i386_cpuid(1U, &eax, &ebx, &ecx, &edx);

	kprintf(
		"i386_init: cpu vendor %s, max leaf %u, family %u model %u stepping %u\n",
		vendor,
		max_leaf,
		(eax >> 8U) & 0xFU,
		(eax >> 4U) & 0xFU,
		eax & 0xFU
	);
}

/*
 * Copy the value of a "name=value" token from the Multiboot command line.
 * QEMU prefixes the line with the kernel's own path, so scan token by token.
 */
static bool i386_cmdline_value(const char *cmdline, const char *name, char *value, uint32_t capacity)
{
	if (cmdline == 0) return false;

	uint32_t name_length = 0U;

	while (name[name_length] != '\0') name_length++;

	while (*cmdline != '\0') {
		while (*cmdline == ' ') cmdline++;

		const char *token = cmdline;

		while (*cmdline != '\0' && *cmdline != ' ') cmdline++;

		uint32_t token_length = (uint32_t)(cmdline - token);

		if (token_length <= name_length || token[name_length] != '=') continue;

		uint32_t index = 0U;

		while (index < name_length && token[index] == name[index]) index++;
		if (index != name_length) continue;

		uint32_t length = token_length - name_length - 1U;

		if (length >= capacity) length = capacity - 1U;

		for (index = 0U; index < length; index++) value[index] = token[name_length + 1U + index];
		value[length] = '\0';
		return true;
	}

	return false;
}

void i386_init(uint32_t magic, const multiboot_info_t *info)
{
	uint64_t frequency = timer_calibrate();

	kprintf("%s [%s]\n", NXU_KERNEL_VERSION, sizeof(void *) == 8U ? "x86_64" : "i386");

	if (frequency != 0ULL) {
		/* kprintf has no field widths, so pad the kHz remainder by hand. */
		uint32_t khz = (uint32_t)(frequency / 1000ULL % 1000ULL);

		kprintf(
			"i386_init: tsc %llu Hz (%llu.%c%c%c MHz)\n",
			(unsigned long long)frequency,
			(unsigned long long)(frequency / 1000000ULL),
			(char)('0' + khz / 100U),
			(char)('0' + khz / 10U % 10U),
			(char)('0' + khz % 10U)
		);
	} else {
		kputln("i386_init: tsc calibration failed");
	}

	i386_print_cpu();

	kprintf(
		"i386_init: kernel image %p - %p (%u KiB)\n",
		(void *)__kernel_start,
		(void *)__kernel_end,
		(uint32_t)((uintptr_t)__kernel_end - (uintptr_t)__kernel_start) / 1024U
	);

	if (magic != MULTIBOOT_BOOTLOADER_MAGIC || info == 0) {
		kprintf("i386_init: not entered from a Multiboot loader (magic 0x%x)\n", magic);
	} else {
		if ((info->flags & MULTIBOOT_INFO_MEMORY) != 0U) {
			kprintf(
				"i386_init: memory %u KiB lower, %u KiB upper\n",
				info->mem_lower,
				info->mem_upper
			);
		}

		if ((info->flags & MULTIBOOT_INFO_CMDLINE) != 0U && info->cmdline != 0U) {
			kprintf("i386_init: boot-args \"%s\"\n", (const char *)(uintptr_t)info->cmdline);
		}
	}

	i386_gdt_init();
	i386_idt_init();

	if (!i386_trap_self_test()) {
		kputln("i386_init: trap self-test failed");
		for (;;) {
			__asm__ volatile("cli\n\thlt");
		}
	}

	kputln("i386_init: trap self-test passed (breakpoint resume, int 0x80 frame write-back)");

	if (magic == MULTIBOOT_BOOTLOADER_MAGIC && info != 0 && (info->flags & MULTIBOOT_INFO_CMDLINE) != 0U) {
		char test[I386_CMDLINE_VALUE_MAX];

		if (i386_cmdline_value((const char *)(uintptr_t)info->cmdline, "trap-test", test, sizeof(test))) {
			i386_trap_test(test);
		}
	}

	kputln("i386_init: machine layer online; vm, scheduler and userland are not ported yet");

	/* Lets an automated run end cleanly under QEMU's isa-debug-exit device. */
	if (magic == MULTIBOOT_BOOTLOADER_MAGIC && info != 0 && (info->flags & MULTIBOOT_INFO_CMDLINE) != 0U) {
		char exit_value[I386_CMDLINE_VALUE_MAX];

		if (i386_cmdline_value((const char *)(uintptr_t)info->cmdline, "qemu-exit", exit_value, sizeof(exit_value))) {
			outb(0xF4U, 0x00U);
		}
	}

	for (;;) {
		__asm__ volatile("cli\n\thlt");
	}
}
