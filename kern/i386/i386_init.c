/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/i386_init.c
 *
 * First C code on an x86 boot. Brings up the serial console and the
 * counter, reports what the bootloader and CPU told us, and stops. The rest
 * of kern_init (memory management, scheduler, userland) is not ported to
 * this architecture yet; this is the seam it will be entered from.
 */

#include <kern/i386/boot_info.h>
#include <kern/i386/gdt.h>
#include <kern/i386/idt.h>
#include <kern/i386/io.h>
#include <kern/i386/multiboot.h>
#include <kern/i386/timer.h>
#include <kern/i386/trap.h>

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

	kprintf("i386_init: cpu vendor %s\n", vendor);
	kprintf("i386_init: cpu max leaf %u\n", max_leaf);
	kprintf("i386_init: cpu family %u\n", (eax >> 8U) & 0xFU);
	kprintf("i386_init: cpu model %u\n", (eax >> 4U) & 0xFU);
	kprintf("i386_init: cpu stepping %u\n", eax & 0xFU);
}

static i386_boot_info_t g_boot_info;

const i386_boot_info_t *i386_boot_info(void)
{
	return &g_boot_info;
}

/*
 * QEMU prefixes the command line with the kernel's own path, so scan token
 * by token rather than assuming the arguments start at the beginning.
 */
bool i386_boot_arg(const char *name, char *value, uint32_t capacity)
{
	const char *cmdline = g_boot_info.cmdline;

	if (cmdline == 0 || capacity == 0U) return false;

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

bool i386_boot_test_requested(const char *name)
{
	char value[I386_CMDLINE_VALUE_MAX];

	if (!i386_boot_arg("test", value, sizeof(value))) return false;

	uint32_t index = 0U;

	while (value[index] != '\0' && name[index] == value[index]) index++;
	return value[index] == '\0' && name[index] == '\0';
}

void i386_shutdown(uint8_t code)
{
	/* QEMU's isa-debug-exit exits with (code << 1) | 1; harmless when absent. */
	outb(0xF4U, code);

	for (;;) {
		__asm__ volatile("cli\n\thlt");
	}
}

/* Weak defaults: an area that is not ported yet succeeds without doing anything. */
#define I386_WEAK_PHASE(name) \
	__attribute__((weak)) bool name(const i386_boot_info_t *boot) \
	{ \
		(void)boot; \
		return true; \
	}

I386_WEAK_PHASE(i386_init_platform)
I386_WEAK_PHASE(i386_init_interrupts)
I386_WEAK_PHASE(i386_init_vm)
I386_WEAK_PHASE(i386_init_kernel)
I386_WEAK_PHASE(i386_init_threads)
I386_WEAK_PHASE(i386_init_drivers)
I386_WEAK_PHASE(i386_init_userland)
I386_WEAK_PHASE(i386_init_run)

I386_WEAK_PHASE(i386_init_platform_selftest)
I386_WEAK_PHASE(i386_init_interrupts_selftest)
I386_WEAK_PHASE(i386_init_vm_selftest)
I386_WEAK_PHASE(i386_init_kernel_selftest)
I386_WEAK_PHASE(i386_init_threads_selftest)
I386_WEAK_PHASE(i386_init_drivers_selftest)
I386_WEAK_PHASE(i386_init_userland_selftest)

typedef struct {
	const char *name;
	bool (*run)(const i386_boot_info_t *boot);
	bool (*selftest)(const i386_boot_info_t *boot);
} i386_phase_t;

static const i386_phase_t g_phases[] = {
	{ "platform", i386_init_platform, i386_init_platform_selftest },
	{ "interrupts", i386_init_interrupts, i386_init_interrupts_selftest },
	{ "vm", i386_init_vm, i386_init_vm_selftest },
	{ "kernel", i386_init_kernel, i386_init_kernel_selftest },
	{ "threads", i386_init_threads, i386_init_threads_selftest },
	{ "drivers", i386_init_drivers, i386_init_drivers_selftest },
	{ "userland", i386_init_userland, i386_init_userland_selftest }
};

static void i386_fail(const char *phase, const char *what)
{
	kprintf("i386_init: %s %s failed\n", phase, what);
	i386_shutdown(0x01U);
}

void i386_init(uint32_t magic, const multiboot_info_t *info)
{
	g_boot_info.magic = magic;
	g_boot_info.multiboot = info;
	g_boot_info.cmdline = 0;

	if (magic == MULTIBOOT_BOOTLOADER_MAGIC && info != 0 && (info->flags & MULTIBOOT_INFO_CMDLINE) != 0U && info->cmdline != 0U) {
		g_boot_info.cmdline = (const char *)(uintptr_t)info->cmdline;
	}

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

	{
		char test[I386_CMDLINE_VALUE_MAX];

		if (i386_boot_arg("trap-test", test, sizeof(test))) i386_trap_test(test);
	}

	for (uint32_t index = 0U; index < sizeof(g_phases) / sizeof(g_phases[0]); index++) {
		const i386_phase_t *phase = &g_phases[index];

		if (!phase->run(&g_boot_info)) i386_fail(phase->name, "phase");

		if (i386_boot_test_requested(phase->name)) {
			kprintf("i386_init: running %s self-test\n", phase->name);

			if (!phase->selftest(&g_boot_info)) i386_fail(phase->name, "self-test");

			kprintf("i386_init: %s self-test passed\n", phase->name);
		}
	}

	kputln("i386_init: boot phases complete");

	i386_init_run(&g_boot_info);

	/* Lets an automated run end cleanly under QEMU's isa-debug-exit device. */
	{
		char exit_value[I386_CMDLINE_VALUE_MAX];

		if (i386_boot_arg("qemu-exit", exit_value, sizeof(exit_value))) i386_shutdown(0x00U);
	}

	for (;;) {
		__asm__ volatile("cli\n\thlt");
	}
}
