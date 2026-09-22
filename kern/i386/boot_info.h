/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/boot_info.h
 *
 * What the early boot path knows, and the phase hooks i386_init() calls.
 *
 * Bring-up is a fixed sequence of phases. Each phase is a hook with a weak
 * do-nothing default in i386_init.c; the subsystem that owns a phase supplies
 * the strong definition in its own file. That lets every area be built and
 * booted on its own before its neighbours exist, and keeps i386_init.c out of
 * the set of files two areas would both need to edit.
 *
 * Each phase returns true on success. A false return is fatal: i386_init()
 * reports the phase and stops. A phase that is not implemented yet simply
 * keeps the weak default, which succeeds without doing anything.
 *
 * Every phase is followed by an optional self-test hook of the same name
 * with a _selftest suffix. It runs only when the boot argument
 * "test=<phase>" names it (see i386_boot_arg), so a normal boot pays nothing
 * for tests. A self-test returns true on pass; it prints its own detail.
 *
 * Order, and what each phase may assume from those before it:
 *
 *   platform     multiboot memory map and PCI/legacy device discovery, into
 *                platform_t. Nothing else is up: serial console, GDT, IDT.
 *   interrupts   8259 PIC remapped and masked, PIT tick, irq_init()
 *                dispatch, timer interrupt. Interrupts stay disabled.
 *   vm           paging on, higher-half kernel, pmm, vmm, vm_kern.
 *   kernel       heap, ipc, proc/task tables, vfs core.
 *   threads      thread/sched bootstrap, context switch, preemption.
 *   smp          MP table scan, Local APIC, secondary CPUs (kern/i386/smp.c);
 *                a uniprocessor MP table (or none) leaves this a no-op. The
 *                8259 PIC still delivers every device IRQ to the boot CPU
 *                only (see doc/i386/smp.md's known gaps); the Local APIC is
 *                used only for IPIs and INIT-SIPI-SIPI.
 *   drivers      virtio-pci block/input, block layer, filesystem mount.
 *   userland     ring 3 entry, syscalls, load and start bootd.
 */

#ifndef NXU_KERN_I386_BOOT_INFO_H
#define NXU_KERN_I386_BOOT_INFO_H

#include <kern/i386/multiboot.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t magic;
	const multiboot_info_t *multiboot;
	const char *cmdline;
} i386_boot_info_t;

/* The boot information captured at entry; valid for the whole boot. */
const i386_boot_info_t *i386_boot_info(void);

/*
 * Copy the value of a "name=value" token from the boot command line.
 * Returns false if the token is absent. The value is truncated to fit.
 */
bool i386_boot_arg(const char *name, char *value, uint32_t capacity);

/* True if the command line contains "test=<name>". */
bool i386_boot_test_requested(const char *name);

bool i386_init_platform(const i386_boot_info_t *boot);
bool i386_init_interrupts(const i386_boot_info_t *boot);
bool i386_init_vm(const i386_boot_info_t *boot);
bool i386_init_kernel(const i386_boot_info_t *boot);
bool i386_init_threads(const i386_boot_info_t *boot);
bool i386_init_smp(const i386_boot_info_t *boot);
bool i386_init_drivers(const i386_boot_info_t *boot);
bool i386_init_userland(const i386_boot_info_t *boot);

/*
 * Runs after every phase (and its self-test) has completed. This is where a
 * boot that has userland work to do hands the CPU to the scheduler: the boot
 * context yields until it is told to stop, so user processes actually run.
 * The default returns at once; a return lets i386_init() carry on to its
 * qemu-exit / halt tail.
 */
bool i386_init_run(const i386_boot_info_t *boot);

bool i386_init_platform_selftest(const i386_boot_info_t *boot);
bool i386_init_interrupts_selftest(const i386_boot_info_t *boot);
bool i386_init_vm_selftest(const i386_boot_info_t *boot);
bool i386_init_kernel_selftest(const i386_boot_info_t *boot);
bool i386_init_threads_selftest(const i386_boot_info_t *boot);
bool i386_init_smp_selftest(const i386_boot_info_t *boot);
bool i386_init_drivers_selftest(const i386_boot_info_t *boot);
bool i386_init_userland_selftest(const i386_boot_info_t *boot);

/* Stop the machine; ends a QEMU run when isa-debug-exit is present. */
void i386_shutdown(uint8_t code) __attribute__((noreturn));

#endif
