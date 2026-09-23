/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/boot_test.c
 *
 * See kern/tests/boot_test.h.
 */

#include <kern/tests/boot_test.h>

#include <kern/aqua/window_server.h>
#include <kern/boot/boot_args.h>
#include <kern/boot/splash.h>
#include <kern/console/bootlog.h>
#include <kern/console/console.h>
#include <kern/console/ioregistry.h>
#include <kern/process/proc.h>
#include <kern/tests/about_sevos_process_test.h>
#include <kern/tests/fault_process_test.h>
#include <kern/tests/ipc_process_test.h>
#include <kern/tests/process_control_test.h>
#include <kern/tests/socket_process_test.h>
#include <kern/tests/sound_test.h>
#include <kern/tests/tep_mailbox_test.h>
#include <kern/tests/thread_process_test.h>
#include <kern/tests/unified_boot.h>
#include <kern/tests/windowserver_process_test.h>
#include <kern/tests/xamethyst_process_test.h>
#include <drivers/video/ui_service_host.h>
#include <kern/arm64/gic.h>
#include <kern/machine/machine_routines.h>
#include <vfs/btrfs/btrfs_selftest.h>
#include <vfs/ext4.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * boot_test_fail
 *
 * A boot test that fails has nothing to return to: log it and stop.
 */
static __attribute__((noreturn, unused))
void boot_test_fail(const char *message)
{
	kputln(message);

	for (;;) {
		__asm__ volatile("wfe");
	}
}


#if defined(NXU_JOURNAL_CRASH_TEST)
static const char g_jbd2_recovery_message[] = "NXU JBD2 committed metadata survived the crash.\n";

/*
 * boot_test_run_jbd2_crash:
 *
 * Create a stable empty file, arm the journal crash point, then perform one
 * write whose metadata reaches a durable JBD2 commit record but is not
 * checkpointed home. The kernel halts immediately afterward. A normal reboot
 * must replay the transaction and recover the file size/block mapping.
 */
static void boot_test_run_jbd2_crash(void)
{
	proc_t kernel_proc = proc_kernel();
	if (kernel_proc == 0) boot_test_fail("jbd2: crash-test kernel proc missing");
	filedesc_t filedesc = &kernel_proc->p_fd;
	uint32_t descriptor;

	vfs_status_t status = vfs_open(
		filedesc,
		"/disk/NXU/journal-recovery.txt",
		VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
		&descriptor
	);
	if (status != VFS_STATUS_OK) boot_test_fail("jbd2: crash-test file preparation failed");
	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) boot_test_fail("jbd2: crash-test close failed");
	if (!ext4_debug_arm_journal_crash()) boot_test_fail("jbd2: failed to arm crash point");

	status = vfs_open(
		filedesc,
		"/disk/NXU/journal-recovery.txt",
		VFS_OPEN_WRITE,
		&descriptor
	);
	if (status != VFS_STATUS_OK) boot_test_fail("jbd2: crash-test reopen failed");

	uint64_t written = 0ULL;
	status = vfs_write(
		filedesc,
		descriptor,
		g_jbd2_recovery_message,
		sizeof(g_jbd2_recovery_message) - 1ULL,
		&written
	);
	(void)vfs_close(filedesc, descriptor);
	if (status != VFS_STATUS_IO_ERROR) boot_test_fail("jbd2: crash point did not stop checkpoint");
	if (!ext4_debug_journal_crash_reached()) boot_test_fail("jbd2: durable crash point was not reached");

	kputln("jbd2: crash-test transaction committed");
	kputln("jbd2: durable commit verified before simulated power loss");
	kputln("jbd2: home metadata intentionally not checkpointed");
	kputln("jbd2: halt complete; close QEMU and run make run");
	ml_irq_disable();
	for (;;) __asm__ volatile("wfe");
}
#endif

void boot_test_graphical(display_device_t *boot_display)
{
	(void)boot_display;

#if defined(NXU_WINDOWSERVER_BOOT_TEST) && !defined(NXU_AQUA_BOOT_TEST)
	/* WindowServer-only bring-up remains available as a compositor smoke test. */
	if (boot_display != 0) ioreg_dump();
	if (boot_display != 0 && !boot_splash_finish()) boot_test_fail("boot_splash_finish: completion failed");
	nxu_boot_log_ui_handoff();
	arm64_enable_irqs();

	if (!windowserver_bootstrap()) {
		boot_test_fail("panic: WindowServer bootstrap failed");
	}

	for (;;) {
		__asm__ volatile("wfe");
	}
#endif

#if defined(NXU_AQUA_BOOT_TEST)
	if (boot_display != 0) ioreg_dump();
	if (boot_display != 0 && !boot_splash_finish()) boot_test_fail("boot_splash_finish: completion failed");
	nxu_boot_log_ui_handoff();
	arm64_enable_irqs();

	if (!windowserver_bootstrap()) {
		boot_test_fail("panic: WindowServer bootstrap failed");
	}
	if (!ui_service_bootstrap()) {
		boot_test_fail("panic: interactive session failed");
	}

	for (;;) {
		__asm__ volatile("wfe");
	}
#endif

#if defined(NXU_UI_SERVICE_BOOT_TEST)
	/* UIService requires WindowServer for surface submission. */
	if (boot_display != 0) ioreg_dump();
	if (boot_display != 0 && !boot_splash_finish()) boot_test_fail("boot_splash_finish: completion failed");
	nxu_boot_log_ui_handoff();
	arm64_enable_irqs();

#if defined(NXU_WINDOWSERVER)
	if (!windowserver_bootstrap()) boot_test_fail("panic: WindowServer bootstrap failed");
#endif
	if (!ui_service_bootstrap()) boot_test_fail("panic: interactive session failed");

	for (;;) __asm__ volatile("wfe");
#endif
}

void boot_test_storage(display_device_t *boot_display)
{
	(void)boot_display;

	/*
	 * Boot argument only ("-append btrfs-test=<spec>"): mount Btrfs
	 * fixtures attached as extra virtio-blk devices after the root disk
	 * and check them (vfs/btrfs/btrfs_selftest.h). Absent, this does
	 * nothing: the filesystem is not even registered.
	 */
	char btrfs_spec[256];

	if (boot_arg_value("btrfs-test", btrfs_spec, sizeof(btrfs_spec))) {
		bool btrfs_ok = btrfs_selftest_run(btrfs_spec);

		kputs(btrfs_ok ? "btrfs-test: passed; powering off (test build, no bootd)\n" : "btrfs-test: FAILED; powering off\n");

		/* PSCI SYSTEM_OFF; the test script also stops QEMU on the result line. */
		register uint64_t function __asm__("x0") = 0x84000008ULL;
		__asm__ volatile("hvc #0" : "+r"(function) : : "memory");

		for (;;) __asm__ volatile("wfe");
	}

#if defined(NXU_UNIFIED_BOOT_TEST) || defined(NXU_DESKTOP_BOOT)
	/* Nothing halts: the boot goes on to start bootd, and boot_test_unified takes over later. */
	unified_boot_prepare();
#endif

#if defined(NXU_JOURNAL_CRASH_TEST)
	boot_test_run_jbd2_crash();
#endif

#if defined(NXU_IPC_PROCESS_TEST)
	if (!ipc_process_test()) boot_test_fail("ipc_process_test: failed");
	kputln("ipc_process_test: passed; halting (test build, no bootd)");
	for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_THREAD_PROCESS_TEST)
	if (!thread_process_test()) boot_test_fail("thread_process_test: failed");
	kputln("thread_process_test: passed; halting (test build, no bootd)");
	for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_FAULT_PROCESS_TEST)
	if (!fault_process_test()) boot_test_fail("fault_process_test: failed");
	kputln("fault_process_test: passed; halting (test build, no bootd)");
	for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_PROCESS_CONTROL_TEST)
	if (!process_control_test()) boot_test_fail("process_control_test: failed");
	kputln("process_control_test: passed; halting (test build, no bootd)");
	for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_SOUND_TEST)
	if (!sound_test_run()) boot_test_fail("sound_test: failed");
	kputln("sound_test: passed; halting (test build, no bootd)");
	for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_TEP_MAILBOX_TEST)
	if (!tep_mailbox_test_run()) boot_test_fail("tep_mailbox_test: failed");
	kputln("tep_mailbox_test: passed; halting (test build, no bootd)");
	for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_SOCKET_PROCESS_TEST)
	if (!socket_process_test()) boot_test_fail("socket_process_test: failed");
	kputln("socket_process_test: passed; halting (test build, no bootd)");
	for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_XAMETHYST_PROCESS_TEST)
	if (boot_display != 0 && !boot_splash_finish()) boot_test_fail("boot_splash_finish: completion failed");

	if (!xamethyst_process_test()) boot_test_fail("amethyst_test_process: failed");
	kputln("amethyst_test_process: passed; halting (test build)");
	for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_WINDOWSERVER_PROCESS_TEST)
	/*
	 * Hand the framebuffer over to WindowServer completely, exactly
	 * like the normal boot path does right before spawning bootd
	 * (see kern_launch_init_process() in kern/kern_init.c): once
	 * unregistered, no further kprintf/kputln output
	 * touches the graphical console, leaving WindowServer's own
	 * present() calls as the only thing drawing to the screen.
	 */
	if (boot_display != 0 && !boot_splash_finish()) boot_test_fail("boot_splash_finish: completion failed");

	if (!windowserver_process_test()) boot_test_fail("windowserver_process_test: failed");
	kputln("windowserver_process_test: passed; halting (test build)");
	for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_ABOUT_SEVOS_PROCESS_TEST)
	if (boot_display != 0 && !boot_splash_finish()) boot_test_fail("boot_splash_finish: completion failed");

	if (!about_sevos_process_test()) boot_test_fail("about_sevos_process_test: failed");
	kputln("about_sevos_process_test: passed; halting (test build)");
	for (;;) __asm__ volatile("wfe");
#endif
}

void boot_test_unified(void)
{
#if defined(NXU_UNIFIED_BOOT_TEST)
	unified_boot_run();
#elif defined(NXU_DESKTOP_BOOT)
	/* Voyager, without the live regression run sharing its CPU -- see unified_boot.h. */
	unified_boot_run_ui_only();
#endif
}
