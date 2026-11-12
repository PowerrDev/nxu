/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/kernel_init.c
 *
 * The `kernel` boot phase (see boot_info.h): the machine-independent
 * services every later phase builds on, in the order kern/kern_init.c brings
 * them up on arm64. Boot arguments, the NXPC transport, the process
 * manager and the virtual filesystem with a ramfs root. The heap is already
 * up (vm phase). Devices, the scheduler and the disk come in later phases.
 */

#include <kern/i386/boot_info.h>
#include <kern/acpi/platform.h>

#include <kern/boot/boot_args.h>
#include <kern/boot/nvram.h>
#include <kern/console/console.h>
#include <kern/ipc/ipc_init.h>
#include <kern/memory/heap.h>
#include <kern/process/proc.h>
#include <kern/tests/ipc_test.h>
#include <vfs/devfs.h>
#include <vfs/ext4.h>
#include <vfs/file.h>
#include <vfs/ramfs.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char g_kernel_test_message[] = "NXU i386 ramfs round trip";

bool
i386_init_kernel(const i386_boot_info_t *boot)
{
  kputln("heap_init: initializing heap");

  /* The VM phase leaves the heap to this phase. A second init is harmless on i386. */
	if (!heap_init()) {
		kputln("heap_init: failed to initialize heap");
		return false;
	}

	kputln("heap_init: kernel heap ready");
	heap_dump();

  kputln("nvram_bootstrap_bootargs: initializing nvram_bootstrap_bootargs");
	/*
	 * x86 has no device tree: the Multiboot command line is the boot-args
	 * source. An empty line is still a valid (empty) boot-args view.
	 */
	if (!nvram_bootstrap_bootargs(boot->cmdline != 0 ? boot->cmdline : "")) {
		kputln("nvram_bootstrap_bootargs: nvram_bootstrap_bootargs failed");
		return false;
	}

  kputln("boot_args_init: initializing boot args");
	if (!boot_args_init()) {
		kputln("boot_args_init: boot_args_init failed");
		return false;
	}

	/*
	 * The heap and the boot arguments are all the display still needed: bring
	 * the screen up now, so the rest of the boot is visible on it.
	 */
	(void)i386_init_display(boot);

  kputln("nxu_acpi_discover: discovering ACPI");
	nxu_acpi_discover(boot);

  kputln("ipc_init: initializing XPC");
	ipc_init();
	kputln("ipc_init: NXPC kernel transport ready");

  kputln("proc_bootstrap: bootstrapping process manager");
	if (!proc_bootstrap() || proc_kernel() == 0) {
		kputln("proc_bootstrap: process manager unavailable");
		return false;
	}

	kprintf("proc_bootstrap: process manager ready, kernel PID %u\n", (unsigned)proc_kernel()->p_ident.pid);

  kputln("vfs_init: initializing virtual filesystem");
  kputln("ramfs_register: registering ram filesystem");
  kputln("ext4_register: registering ext4");
  kputln("devfs_register: registering devfs");
	if (!vfs_init() || !ramfs_register() || !ext4_register() || !devfs_register()) {
		kputln("vfs_init: vfs registration failed");
		return false;
	}

	vfs_status_t status = vfs_mount("ramfs", 0, "/");

	if (status != VFS_STATUS_OK) {
		kprintf("vfs_init: root ramfs mount failed: %s\n", vfs_status_name(status));
		return false;
	}

  kputln("vfs_mkdir: creating volumes directory");
	status = vfs_mkdir("/Volumes");

  kputln("vfs_mkdir: creating root directory");
	if (status == VFS_STATUS_OK) status = vfs_mkdir("/Volumes/mainOS");

	if (status != VFS_STATUS_OK) {
		kprintf("vfs_mkdir: /Volumes/mainOS mountpoint creation failed: %s\n", vfs_status_name(status));
		return false;
	}

  kputln("vfs_mount: mounting devfs at /dev");
	if (vfs_mkdir("/dev") != VFS_STATUS_OK || vfs_mount("devfs", 0, "/dev") != VFS_STATUS_OK) {
		kputln("vfs_mount: devfs mount at /dev failed");
		return false;
	}

	kputln("vfs_mount: ramfs mounted at /, /Volumes/mainOS mountpoint created");
	kputln("vfs_mount: devfs mounted at /dev");

	if (!devfs_self_test()) return false;
	vfs_dump();
	return true;
}

/* Write a file into the ramfs root through the kernel process's descriptors and read it back. */
static bool
kernel_test_ramfs(void)
{
	filedesc_t filedesc = &proc_kernel()->p_fd;
	uint32_t descriptor;

	if (vfs_mkdir("/tmp") != VFS_STATUS_OK) return false;

	if (vfs_open(filedesc, "/tmp/i386", VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
				 &descriptor) != VFS_STATUS_OK) {
		return false;
	}

	uint64_t size = sizeof(g_kernel_test_message) - 1ULL;
	uint64_t transferred = 0ULL;
	bool ok = vfs_write(filedesc, descriptor, g_kernel_test_message, size, &transferred) == VFS_STATUS_OK &&
			  transferred == size;

	ok = ok && vfs_seek(filedesc, descriptor, 0ULL) == VFS_STATUS_OK;

	char buffer[sizeof(g_kernel_test_message)];

	memset(buffer, 0, sizeof(buffer));
	ok = ok && vfs_read(filedesc, descriptor, buffer, size, &transferred) == VFS_STATUS_OK && transferred == size;
	ok = ok && memcmp(buffer, g_kernel_test_message, (size_t)size) == 0;

	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) ok = false;

	return ok && filedesc->fd_open_count == 0U;
}

bool
i386_init_kernel_selftest(const i386_boot_info_t *boot)
{
	bool ok = true;

	if (!ipc_self_test()) {
		kputln("ipc_self_test: FAIL NXPC transport self-test");
		ok = false;
	}

	if (!ipc_space_self_test()) {
		kputln("ipc_space_self_test: FAIL per-process name table self-test");
		ok = false;
	}

	if (!kernel_test_ramfs()) {
		kputln("kernel_test_ramfs: FAIL ramfs write/read round trip");
		ok = false;
	}

	/* The boot-args view must be the Multiboot command line we were given. */
	if (boot->cmdline != 0 && (strlen(boot_args_raw()) < strlen(boot->cmdline) ||
							   memcmp(boot_args_raw(), boot->cmdline, strlen(boot->cmdline)) != 0)) {
		kputln("boot_args_raw: FAIL boot-args do not carry the command line");
		ok = false;
	}

	if (ok) {
    kputln("ipc_self_test: XPC ok");
    kputln("ipc_space_self_test name tables ok");
    kputln("kernel_test_ramfs: ramfs ok");
    kputln("boot_args_raw: boot-args ok");
  } 

	return ok;
}
