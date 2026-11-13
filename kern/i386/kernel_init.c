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
#include <kern/tty/tty.h>
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
  kprintf("initializing heap\n");

  /* The VM phase leaves the heap to this phase. A second init is harmless on i386. */
	if (!heap_init()) {
		kprintf_for(heap_init, "failed to initialize heap\n");
		return false;
	}

	kprintf("kernel heap ready\n");
	heap_dump();

  kprintf("initializing nvram_bootstrap_bootargs\n");
	/*
	 * x86 has no device tree: the Multiboot command line is the boot-args
	 * source. An empty line is still a valid (empty) boot-args view.
	 */
	if (!nvram_bootstrap_bootargs(boot->cmdline != 0 ? boot->cmdline : "")) {
		kprintf("nvram_bootstrap_bootargs failed\n");
		return false;
	}

  kprintf("initializing boot args\n");
	if (!boot_args_init()) {
		kprintf("boot_args_init failed\n");
		return false;
	}

	/*
	 * The heap and the boot arguments are all the display still needed: bring
	 * the screen up now, so the rest of the boot is visible on it.
	 */
	(void)i386_init_display(boot);

  kprintf("discovering ACPI\n");
	nxu_acpi_discover(boot);

  kprintf("initializing XPC\n");
	ipc_init();
	kprintf("NXPC kernel transport ready\n");

  kprintf("bootstrapping process manager\n");
	if (!proc_bootstrap() || proc_kernel() == 0) {
		kprintf("process manager unavailable\n");
		return false;
	}

	kprintf("process manager ready, kernel PID %u\n", (unsigned)proc_kernel()->p_ident.pid);

  kprintf("initializing virtual filesystem\n");
  kprintf("registering ram filesystem\n");
  kprintf("registering ext4\n");
  kprintf("registering devfs\n");
	if (!vfs_init() || !ramfs_register() || !ext4_register() || !devfs_register()) {
		kprintf("vfs registration failed\n");
		return false;
	}

	vfs_status_t status = vfs_mount("ramfs", 0, "/");

	if (status != VFS_STATUS_OK) {
		kprintf("root ramfs mount failed: %s\n", vfs_status_name(status));
		return false;
	}

  kprintf("creating volumes directory\n");
	status = vfs_mkdir("/Volumes");

  kprintf("creating root directory\n");
	if (status == VFS_STATUS_OK) status = vfs_mkdir("/Volumes/mainOS");

	if (status != VFS_STATUS_OK) {
		kprintf("/Volumes/mainOS mountpoint creation failed: %s\n", vfs_status_name(status));
		return false;
	}

  kprintf("mounting devfs at /dev\n");
	if (vfs_mkdir("/dev") != VFS_STATUS_OK || vfs_mount("devfs", 0, "/dev") != VFS_STATUS_OK) {
		kprintf("devfs mount at /dev failed\n");
		return false;
	}

	kprintf("ramfs mounted at /, /Volumes/mainOS mountpoint created\n");
	kprintf("devfs mounted at /dev\n");

	if (!devfs_self_test()) return false;

	tty_init();
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
		kprintf("FAIL NXPC transport self-test\n");
		ok = false;
	}

	if (!ipc_space_self_test()) {
		kprintf("FAIL per-process name table self-test\n");
		ok = false;
	}

	if (!kernel_test_ramfs()) {
		kprintf("FAIL ramfs write/read round trip\n");
		ok = false;
	}

	/* The boot-args view must be the Multiboot command line we were given. */
	if (boot->cmdline != 0 && (strlen(boot_args_raw()) < strlen(boot->cmdline) ||
							   memcmp(boot_args_raw(), boot->cmdline, strlen(boot->cmdline)) != 0)) {
		kprintf("FAIL boot-args do not carry the command line\n");
		ok = false;
	}

	if (ok) {
    kprintf("XPC ok\n");
    kputln("ipc_space_self_test name tables ok");
    kprintf("ramfs ok\n");
    kprintf("boot-args ok\n");
  } 

	return ok;
}
