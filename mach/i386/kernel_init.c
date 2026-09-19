/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/kernel_init.c
 *
 * The `kernel` boot phase (see boot_info.h): the machine-independent
 * services every later phase builds on, in the order kern/kern_init.c brings
 * them up on arm64 -- boot arguments, the NXPC transport, the process
 * manager and the virtual filesystem with a ramfs root. The heap is already
 * up (vm phase). Devices, the scheduler and the disk come in later phases.
 */

#include <mach/i386/boot_info.h>

#include <kern/boot/boot_args.h>
#include <kern/boot/nvram.h>
#include <kern/console/console.h>
#include <kern/ipc/ipc_init.h>
#include <kern/memory/heap.h>
#include <kern/process/proc.h>
#include <kern/tests/ipc_test.h>
#include <vfs/ext4.h>
#include <vfs/file.h>
#include <vfs/ramfs.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char g_kernel_test_message[] = "NXU i386 ramfs round trip";

bool i386_init_kernel(const i386_boot_info_t *boot)
{
	/* The VM phase leaves the heap to this phase; a second init is harmless on i386. */
	if (!heap_init()) {
		kputln("i386_init_kernel: heap_init failed");
		return false;
	}

	kputln("i386_init_kernel: kernel heap ready");

	/*
	 * x86 has no device tree: the Multiboot command line is the boot-args
	 * source. An empty line is still a valid (empty) boot-args view.
	 */
	if (!nvram_bootstrap_bootargs(boot->cmdline != 0 ? boot->cmdline : "")) {
		kputln("i386_init_kernel: nvram_bootstrap_bootargs failed");
		return false;
	}

	if (!boot_args_init()) {
		kputln("i386_init_kernel: boot_args_init failed");
		return false;
	}

	ipc_init();
	kputln("i386_init_kernel: NXPC kernel transport ready");

	if (!proc_bootstrap() || proc_kernel() == 0) {
		kputln("i386_init_kernel: process manager unavailable");
		return false;
	}

	kprintf("i386_init_kernel: process manager ready, kernel PID %u\n", (unsigned)proc_kernel()->p_ident.pid);

	if (!vfs_init() || !ramfs_register() || !ext4_register()) {
		kputln("i386_init_kernel: vfs registration failed");
		return false;
	}

	vfs_status_t status = vfs_mount("ramfs", 0, "/");

	if (status != VFS_STATUS_OK) {
		kprintf("i386_init_kernel: root ramfs mount failed: %s\n", vfs_status_name(status));
		return false;
	}

	status = vfs_mkdir("/disk");

	if (status != VFS_STATUS_OK) {
		kprintf("i386_init_kernel: /disk mountpoint creation failed: %s\n", vfs_status_name(status));
		return false;
	}

	kputln("i386_init_kernel: ramfs mounted at /, /disk mountpoint created");
	return true;
}

/* Write a file into the ramfs root through the kernel process's descriptors and read it back. */
static bool kernel_test_ramfs(void)
{
	filedesc_t filedesc = &proc_kernel()->p_fd;
	uint32_t descriptor;

	if (vfs_mkdir("/tmp") != VFS_STATUS_OK) return false;

	if (vfs_open(filedesc, "/tmp/i386", VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, &descriptor) != VFS_STATUS_OK) {
		return false;
	}

	uint64_t size = sizeof(g_kernel_test_message) - 1ULL;
	uint64_t transferred = 0ULL;
	bool ok = vfs_write(filedesc, descriptor, g_kernel_test_message, size, &transferred) == VFS_STATUS_OK && transferred == size;

	ok = ok && vfs_seek(filedesc, descriptor, 0ULL) == VFS_STATUS_OK;

	char buffer[sizeof(g_kernel_test_message)];

	memset(buffer, 0, sizeof(buffer));
	ok = ok && vfs_read(filedesc, descriptor, buffer, size, &transferred) == VFS_STATUS_OK && transferred == size;
	ok = ok && memcmp(buffer, g_kernel_test_message, (size_t)size) == 0;

	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) ok = false;

	return ok && filedesc->fd_open_count == 0U;
}

bool i386_init_kernel_selftest(const i386_boot_info_t *boot)
{
	bool ok = true;

	if (!ipc_self_test()) {
		kputln("i386_init_kernel_selftest: FAIL NXPC transport self-test");
		ok = false;
	}

	if (!ipc_space_self_test()) {
		kputln("i386_init_kernel_selftest: FAIL per-process name table self-test");
		ok = false;
	}

	if (!kernel_test_ramfs()) {
		kputln("i386_init_kernel_selftest: FAIL ramfs write/read round trip");
		ok = false;
	}

	/* The boot-args view must be the Multiboot command line we were given. */
	if (boot->cmdline != 0 && !boot_arg_present("test=kernel")) {
		kputln("i386_init_kernel_selftest: FAIL boot-args do not carry the command line");
		ok = false;
	}

	if (ok) kputln("i386_init_kernel_selftest: NXPC, name tables, ramfs and boot-args ok");

	return ok;
}
