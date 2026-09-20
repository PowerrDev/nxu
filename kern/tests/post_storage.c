/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/post_storage.c
 *
 * The storage half of the power-on self-test: raw block access and the
 * writable ext4 volume. See kern/tests/post.h.
 */

#include <drivers/block/block_device.h>
#include <kern/boot/boot_mode.h>
#include <kern/console/console.h>
#include <kern/process/proc.h>
#include <vfs/ext4.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool post_storage(void);

/*
 * post_test_block_device:
 *
 * Read sector zero through the transport-independent block layer. The test is
 * intentionally read-only so the development disk can later be formatted as
 * ext4 without a kernel bootstrap test modifying filesystem metadata.
 */
static bool post_test_block_device(void)
{
	block_device_t device = block_device_first();

	if (device == 0) return false;

	uint8_t sector[BLOCK_SECTOR_SIZE];

	memset(
		sector,
		0,
		sizeof(sector)
	);

	if (!block_device_read(
		device,
		0ULL,
		1U,
		sector
	)) {
		return false;
	}

	kputs(
		"VirtIOBlockFamily: sector 0 first bytes:"
	);

	for (
		uint32_t index = 0U;
		index < 16U;
		index++
	) {
		kputc(' ');
		kputhex_byte(
			sector[index]
		);
	}

	kputc('\n');

	return true;
}

static const char g_ext4_test_message[] = "Hello from the NXU ext4 driver!\n";
static const char g_ext4_system_message[] = "NXU System volume\n";
static const char g_ext4_write_message[] = "NXU writable ext4 foundation\n";
static const char g_ext4_persist_message[] = "NXU persistent ext4 marker\n";
static const char g_ext4_tail_message[] = "tail-after-sparse-growth";
static const char g_jbd2_recovery_message[] = "NXU JBD2 committed metadata survived the crash.\n";

static bool post_ext4_read_exact(
	filedesc_t filedesc,
	const char *path,
	const char *expected,
	uint64_t size
)
{
	uint32_t descriptor;
	if (vfs_open(filedesc, path, VFS_OPEN_READ, &descriptor) != VFS_STATUS_OK) return false;

	char buffer[128];
	if (size > sizeof(buffer)) {
		(void)vfs_close(filedesc, descriptor);
		return false;
	}
	memset(buffer, 0, sizeof(buffer));

	uint64_t read_size;
	vfs_status_t status = vfs_read(filedesc, descriptor, buffer, size, &read_size);
	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return false;

	if (status != VFS_STATUS_OK || read_size != size) return false;
	for (uint64_t index = 0ULL; index < size; index++) {
		if (buffer[index] != expected[index]) return false;
	}
	return true;
}

/*
 * post_test_ext4:
 *
 * Exercise the complete writable ext4 foundation through VFS. The test covers
 * existing-file reads, inode and block allocation, extent growth, directory
 * insertion, truncate shrink/grow semantics, sparse reads, unlink and a
 * persistent marker which is observed on subsequent boots.
 */
static bool post_test_ext4(void)
{
	proc_t kernel_proc = proc_kernel();
	if (kernel_proc == 0) return false;
	filedesc_t filedesc = &kernel_proc->p_fd;

	if (!post_ext4_read_exact(
		filedesc,
		"/disk/hello.txt",
		g_ext4_test_message,
		sizeof(g_ext4_test_message) - 1ULL
	)) return false;

	if (!post_ext4_read_exact(
		filedesc,
		"/disk/System/README.txt",
		g_ext4_system_message,
		sizeof(g_ext4_system_message) - 1ULL
	)) return false;

	vfs_status_t status = vfs_mkdir("/disk/NXU");
	if (status != VFS_STATUS_OK && status != VFS_STATUS_EXISTS) return false;

	uint32_t descriptor;
	status = vfs_open(
		filedesc,
		"/disk/NXU/write-test.bin",
		VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
		&descriptor
	);
	if (status != VFS_STATUS_OK) return false;

	uint8_t block[1024];
	for (uint32_t chunk = 0U; chunk < 12U; chunk++) {
		for (uint32_t index = 0U; index < sizeof(block); index++) {
			block[index] = (uint8_t)(chunk ^ index);
		}
		uint64_t written;
		status = vfs_write(filedesc, descriptor, block, sizeof(block), &written);
		if (status != VFS_STATUS_OK || written != sizeof(block)) goto fail_close;
	}

	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return false;

	vnode_t vnode;
	status = vfs_lookup("/disk/NXU/write-test.bin", &vnode);
	if (status != VFS_STATUS_OK) return false;
	status = vnode_truncate(vnode, 5000ULL);
	if (status == VFS_STATUS_OK) status = vnode_truncate(vnode, 9000ULL);
	vnode_rele(vnode);
	if (status != VFS_STATUS_OK) return false;

	status = vfs_open(
		filedesc,
		"/disk/NXU/write-test.bin",
		VFS_OPEN_READ | VFS_OPEN_WRITE,
		&descriptor
	);
	if (status != VFS_STATUS_OK) return false;

	if (vfs_seek(filedesc, descriptor, 5000ULL) != VFS_STATUS_OK) goto fail_close;

	uint8_t zeros[128];
	memset(zeros, 0xA5, sizeof(zeros));
	uint64_t read_size;
	status = vfs_read(filedesc, descriptor, zeros, sizeof(zeros), &read_size);
	if (status != VFS_STATUS_OK || read_size != sizeof(zeros)) goto fail_close;
	for (uint32_t index = 0U; index < sizeof(zeros); index++) {
		if (zeros[index] != 0U) goto fail_close;
	}

	if (vfs_seek(filedesc, descriptor, 8192ULL) != VFS_STATUS_OK) goto fail_close;
	uint64_t written;
	status = vfs_write(
		filedesc,
		descriptor,
		g_ext4_tail_message,
		sizeof(g_ext4_tail_message) - 1ULL,
		&written
	);
	if (status != VFS_STATUS_OK || written != sizeof(g_ext4_tail_message) - 1ULL) goto fail_close;
	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return false;

	status = vfs_open(
		filedesc,
		"/disk/NXU/delete-me.txt",
		VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
		&descriptor
	);
	if (status != VFS_STATUS_OK) return false;
	status = vfs_write(
		filedesc,
		descriptor,
		g_ext4_write_message,
		sizeof(g_ext4_write_message) - 1ULL,
		&written
	);
	if (status != VFS_STATUS_OK) goto fail_close;
	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return false;

	if (vfs_unlink("/disk/NXU/delete-me.txt") != VFS_STATUS_OK) return false;
	status = vfs_lookup("/disk/NXU/delete-me.txt", &vnode);
	if (status != VFS_STATUS_NOT_FOUND) {
		if (status == VFS_STATUS_OK) vnode_rele(vnode);
		return false;
	}

	status = vfs_lookup("/disk/NXU/journal-recovery.txt", &vnode);
	if (status == VFS_STATUS_OK) {
		vnode_rele(vnode);
		if (!post_ext4_read_exact(
			filedesc,
			"/disk/NXU/journal-recovery.txt",
			g_jbd2_recovery_message,
			sizeof(g_jbd2_recovery_message) - 1ULL
		)) return false;
		kputln("jbd2: committed crash-test transaction recovered");
	} else if (status != VFS_STATUS_NOT_FOUND) {
		return false;
	}

	status = vfs_lookup("/disk/NXU/persistent.txt", &vnode);
	if (status == VFS_STATUS_OK) {
		vnode_rele(vnode);
		if (!post_ext4_read_exact(
			filedesc,
			"/disk/NXU/persistent.txt",
			g_ext4_persist_message,
			sizeof(g_ext4_persist_message) - 1ULL
		)) return false;
		kputln("IOFilesystemFamily: persistence marker recovered from previous boot");
	} else if (status == VFS_STATUS_NOT_FOUND) {
		status = vfs_open(
			filedesc,
			"/disk/NXU/persistent.txt",
			VFS_OPEN_WRITE | VFS_OPEN_CREATE,
			&descriptor
		);
		if (status != VFS_STATUS_OK) return false;
		status = vfs_write(
			filedesc,
			descriptor,
			g_ext4_persist_message,
			sizeof(g_ext4_persist_message) - 1ULL,
			&written
		);
		if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return false;

		if (status != VFS_STATUS_OK || written != sizeof(g_ext4_persist_message) - 1ULL) return false;
		kputln("IOFilesystemFamily: persistence marker created; reboot to verify persistence");
	} else {
		return false;
	}

	kputs("IOFilesystemFamily: /hello.txt: ");
	kputs(g_ext4_test_message);
	return filedesc->fd_open_count == 0U;

fail_close:
	(void)vfs_close(filedesc, descriptor);
	return false;
}

bool post_storage(void)
{
	if (!post_test_block_device()) {
		kputln("VirtIOBlockFamily: sector read test failed");
		return false;
	}

	if (boot_mode_is_triage_os()) {
		kputln("IOFilesystemFamily: recovery boot; writable filesystem self-test skipped");
		return true;
	}

	if (!post_test_ext4()) {
		kputln("IOFilesystemFamily: writable filesystem self-test failed");
		return false;
	}

	kputln("IOFilesystemFamily: writable filesystem self-test passed");

	if (vfs_sync_all() != VFS_STATUS_OK) {
		kputln("IOVirtualFSDriver filesystem sync failed");
		return false;
	}

	kputln("IOVirtualFSDriver mounted filesystems synchronized");
	return true;
}
