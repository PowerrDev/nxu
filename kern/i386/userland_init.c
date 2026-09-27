/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/userland_init.c
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

#include <kern/i386/boot_info.h>
#include <kern/i386/fs_test.h>
#include <kern/i386/timer.h>

#include <drivers/block/block_device.h>
#include <drivers/tep/tep_mailbox.h>
#include <drivers/video/ui_service_host.h>
#include <kern/aqua/window_server.h>
#include <kern/boot/boot_chime.h>
#include <kern/boot/boot_mode.h>
#include <kern/console/console.h>
#include <kern/loader/elf.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <kern/tests/ipc_process_test.h>
#include <kern/tests/socket_process_test.h>
#include <kern/tests/sound_test.h>
#include <kern/tests/thread_process_test.h>
#include <vfs/btrfs/btrfs_list.h>
#include <vfs/btrfs/btrfs_selftest.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define USERLAND_BOOTD_PATH "/disk/System/Library/CoreServices/bootd"
#define USERLAND_BOOTD_RECOVERY_PATH "/disk/System/Library/CoreServices/bootd.recovery"

static proc_t g_boot_process;

/* Decimal boot-arg value to uint32_t; stops at the first non-digit. */
static uint32_t userland_parse_u32(const char *text)
{
	uint32_t value = 0U;

	for (uint32_t index = 0U; text[index] >= '0' && text[index] <= '9'; index++) {
		value = value * 10U + (uint32_t)(text[index] - '0');
	}

	return value;
}

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

/* Same policy as arm64: prefer bootd, fall back to its recovery image. */
static bool userland_spawn_bootd(void)
{
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

bool i386_init_userland(const i386_boot_info_t *boot)
{
	(void)boot;

	/*
	 * A boot that asks for another phase's self-test (test=drivers, ...) is
	 * exercising that phase in isolation, often on a scratch disk that is
	 * not a filesystem at all. Launching userland from it would turn a
	 * passing self-test into a fatal mount failure, so only test=userland,
	 * test=desktop (the compositor bring-up below, which needs this same
	 * mount) or no test at all goes on to mount and spawn.
	 */
	char selected[16];
	bool selected_present = i386_boot_arg("test", selected, sizeof(selected));

	if (selected_present && strcmp(selected, "userland") != 0 && strcmp(selected, "desktop") != 0) {
		kputln("i386_init_userland: self-test run for another phase, userland launch skipped");
		return true;
	}

	if (block_device_count() == 0U) {
		kputln("i386_init_userland: no block device, staying in kernel-only mode");
		return true;
	}

	/*
	 * "btrfs-ls" and/or "btrfs-cat=<path>": mount ANY Btrfs disk read-only and
	 * print its tree / one file (see vfs/btrfs/btrfs_list.h, make
	 * run-i386-btrfs). Handled before the ext4 root mount so the Btrfs image
	 * can be the only disk. Options: btrfs-dev=<n> (block device, default 0),
	 * btrfs-subvol=<id>, btrfs-noverify=1, btrfs-max=<entries>.
	 */
	char list_value[128];
	btrfs_list_request_t list_request;

	memset(&list_request, 0, sizeof(list_request));
	list_request.list = i386_boot_arg("btrfs-ls", list_value, sizeof(list_value));

	if (i386_boot_arg("btrfs-cat", list_value, sizeof(list_value))) list_request.cat_path = list_value;

	if (list_request.list || list_request.cat_path != 0) {
		char option[24];

		if (i386_boot_arg("btrfs-dev", option, sizeof(option))) list_request.device_index = userland_parse_u32(option);
		if (i386_boot_arg("btrfs-subvol", option, sizeof(option))) list_request.subvolume = userland_parse_u32(option);
		if (i386_boot_arg("btrfs-max", option, sizeof(option))) list_request.max_entries = userland_parse_u32(option);
		list_request.noverify = i386_boot_arg("btrfs-noverify", option, sizeof(option));

		bool list_ok = btrfs_list_run(&list_request);

		kprintf("i386_init_userland: btrfs list %s\n", list_ok ? "done" : "FAILED");
		return list_ok;
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

	/*
	 * "sound-test=1": play the test tone through the VirtIO Sound driver
	 * (kern/tests/sound_test.h); the host then checks what QEMU's audio
	 * backend recorded. Like the tests above it replaces the userland launch.
	 */
	char sound_arg[8];

	if (i386_boot_arg("sound-test", sound_arg, sizeof(sound_arg))) {
		bool sound_ok = sound_test_run();

		kprintf("i386_init_userland: sound-test %s\n", sound_ok ? "passed" : "FAILED");
		return sound_ok;
	}

	/* "fs-test=<write|journal-crash|journal-verify>": see fs_test.h. */
	char fs_mode[24];

	if (i386_boot_arg("fs-test", fs_mode, sizeof(fs_mode))) {
		bool fs_ok = i386_fs_test_run(fs_mode);

		kprintf("i386_init_userland: fs-test %s %s\n", fs_mode, fs_ok ? "passed" : "FAILED");
		return fs_ok;
	}

	/*
	 * "btrfs-test=<spec>[,<spec>...]": mount the Btrfs fixtures attached as
	 * extra disks after the root disk and check them; see
	 * vfs/btrfs/btrfs_selftest.h. Skips the userland launch like fs-test.
	 */
	char btrfs_spec[256];

	if (i386_boot_arg("btrfs-test", btrfs_spec, sizeof(btrfs_spec))) {
		bool btrfs_ok = btrfs_selftest_run(btrfs_spec);

		kprintf("i386_init_userland: btrfs-test %s\n", btrfs_ok ? "passed" : "FAILED");
		return btrfs_ok;
	}

	/*
	 * "test=desktop": the compositor's kernel-embedded bring-up (see
	 * platform/i386/services/ui_service.c and kern/aqua/window_server.c),
	 * the same mechanism arm64's "desktop" test id uses -- WindowServer and
	 * UIService's desktop run directly on this boot context. bootd comes up
	 * first, as on arm64, because the desktop has no apps of its own: bootd
	 * starts the Dock, and the Dock starts the apps in /Applications, each a
	 * process that reaches the desktop through the UI session bridge.
	 * ui_service_bootstrap() blocks for as long as the desktop runs (its loop
	 * yields, which is when bootd and the apps get the CPU) and only returns
	 * false, so its result is this phase's result outright.
	 */
	if (selected_present && strcmp(selected, "desktop") == 0) {
		if (!userland_spawn_bootd()) kputln("i386_init_userland: the desktop starts without bootd, so without the Dock");

		/*
		 * The boot chime, as on arm64: the display is about to show the
		 * desktop and /disk (with Boot_Audio.wav) is mounted, so the chime
		 * thread can start; it plays while the UI loop yields.
		 */
		boot_chime_arm();
		boot_chime_start();

		if (!windowserver_bootstrap()) {
			kputln("i386_init_userland: WindowServer bootstrap failed");
			return false;
		}

		ui_service_set_cooperative(true);
		return ui_service_bootstrap();
	}

	return userland_spawn_bootd();
}

bool i386_init_run(const i386_boot_info_t *boot)
{
	(void)boot;

	/*
	 * Without bootd (no disk, as in make run-i386) there is normally nothing
	 * left to run. The tepOS mailbox monitor is a kernel thread, though, so
	 * keep scheduling while the machine has a mailbox link.
	 */
	if (g_boot_process == 0 && tep_mailbox_link_state() == TEP_LINK_ABSENT) return true;

	char value[16];
	uint64_t limit_us = 0ULL;

	if (i386_boot_arg("run-seconds", value, sizeof(value))) {
		for (uint32_t index = 0U; value[index] >= '0' && value[index] <= '9'; index++) {
			limit_us = limit_us * 10ULL + (uint64_t)(value[index] - '0');
		}

		limit_us *= 1000000ULL;
	}

	uint64_t start_us = timer_get_microseconds();

	kputln(g_boot_process != 0
		? "i386_init_run: root userspace services active, dispatching"
		: "i386_init_run: no userspace; running kernel threads (tepOS mailbox monitor)");

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
