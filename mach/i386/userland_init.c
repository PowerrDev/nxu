/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/userland_init.c
 *
 * The `userland` boot phase and the run hook (see boot_info.h): mount the
 * ext4 root volume, launch bootd as PID 1 and hand the CPU to the scheduler,
 * the same sequence kern/kern_init.c ends with on arm64.
 *
 * i386_init_userland() only prepares: it mounts and spawns, then returns so
 * the phase's self-test and the rest of i386_init() still run. The scheduler
 * is entered from i386_init_run(), which yields from the boot context so
 * bootd and the services it starts get the CPU. With "run-seconds=N" on the
 * command line it stops after N seconds (for automated boots); otherwise it
 * runs for as long as the machine does.
 */

#include <mach/i386/boot_info.h>
#include <mach/i386/timer.h>

#include <drivers/block/block_device.h>
#include <kern/boot/boot_mode.h>
#include <kern/console/console.h>
#include <kern/loader/elf.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <kern/tests/ipc_process_test.h>
#include <kern/tests/socket_process_test.h>
#include <kern/tests/thread_process_test.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define USERLAND_BOOTD_PATH "/disk/System/Library/CoreServices/bootd"
#define USERLAND_BOOTD_RECOVERY_PATH "/disk/System/Library/CoreServices/bootd.recovery"

static proc_t g_boot_process;

static bool userland_run_process_test(const char *name)
{
	bool ok;

	if (strcmp(name, "ipc") == 0) ok = ipc_process_test();
	else if (strcmp(name, "thread") == 0) ok = thread_process_test();
	else if (strcmp(name, "socket") == 0) ok = socket_process_test();
	else {
		kprintf("i386_init_userland: unknown process-test \"%s\"\n", name);
		return false;
	}

	kprintf("i386_init_userland: %s process test %s\n", name, ok ? "passed" : "FAILED");
	return ok;
}

bool i386_init_userland(const i386_boot_info_t *boot)
{
	(void)boot;

	if (block_device_count() == 0U) {
		kputln("i386_init_userland: no block device, staying in kernel-only mode");
		return true;
	}

	boot_mode_init();

	kputln("i386_init_userland: mounting disk0 at /disk");

	vfs_status_t mount_status = vfs_mount("ext4", block_device_first(), "/disk");

	if (mount_status != VFS_STATUS_OK) {
		kprintf("i386_init_userland: ext4 mount failed: %s\n", vfs_status_name(mount_status));
		return false;
	}

	/*
	 * "process-test=<ipc|thread|socket>" runs one of the shared kernel-side
	 * process tests instead of the normal boot. They spawn their own
	 * processes (the IPC one starts its own bootd as PID 1), so bootd must
	 * not already be running.
	 */
	char test[16];

	if (i386_boot_arg("process-test", test, sizeof(test))) return userland_run_process_test(test);

	/* Same policy as arm64: prefer bootd, fall back to its recovery image. */
	kprintf("i386_init_userland: loading %s as PID 1\n", USERLAND_BOOTD_PATH);

	loader_status_t status = loader_spawn(proc_kernel(), USERLAND_BOOTD_PATH, "bootd", &g_boot_process);

	if (status != LOADER_STATUS_OK) {
		kprintf("i386_init_userland: primary bootd unavailable: %s\n", loader_status_name(status));
		status = loader_spawn(proc_kernel(), USERLAND_BOOTD_RECOVERY_PATH, "bootd", &g_boot_process);
	}

	if (status != LOADER_STATUS_OK || g_boot_process == 0) {
		kprintf("i386_init_userland: PID 1 launch failed: %s\n", loader_status_name(status));
		g_boot_process = 0;
		return false;
	}

	if (g_boot_process->p_ident.pid != 1U) {
		kprintf("i386_init_userland: bootd received PID %u, expected 1\n", (unsigned)g_boot_process->p_ident.pid);
		return false;
	}

	kprintf("i386_init_userland: bootd PID %u ready for scheduler dispatch\n", (unsigned)g_boot_process->p_ident.pid);
	return true;
}

bool i386_init_run(const i386_boot_info_t *boot)
{
	(void)boot;

	if (g_boot_process == 0) return true;

	char value[16];
	uint64_t limit_us = 0ULL;

	if (i386_boot_arg("run-seconds", value, sizeof(value))) {
		for (uint32_t index = 0U; value[index] >= '0' && value[index] <= '9'; index++) {
			limit_us = limit_us * 10ULL + (uint64_t)(value[index] - '0');
		}

		limit_us *= 1000000ULL;
	}

	uint64_t start_us = timer_get_microseconds();

	kputln("i386_init_run: root userspace services active, dispatching");

	for (;;) {
		if (!sched_yield()) {
			kputln("i386_init_run: userspace dispatch failed");
			return false;
		}

		if (limit_us != 0ULL && timer_get_microseconds() - start_us >= limit_us) break;
	}

	kputln("i386_init_run: run time elapsed, leaving the scheduler");
	return true;
}
