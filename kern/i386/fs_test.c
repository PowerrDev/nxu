/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/fs_test.c
 *
 * See fs_test.h. The write sequence and the journal crash point mirror what
 * kern/kern_init.c runs on arm64 (kern_test_ext4, kern_run_jbd2_crash_test),
 * on top of the same shared VFS and ext4 code, so a difference in behaviour
 * on i386 is a 32-bit or platform problem rather than a test difference.
 */

#include <kern/i386/fs_test.h>

#include <kern/i386/boot_info.h>

#include <kern/console/console.h>
#include <kern/process/proc.h>
#include <vfs/ext4.h>
#include <vfs/file.h>
#include <vfs/vfs.h>
#include <vfs/vnode.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char g_hello_message[] = "Hello from the NXU ext4 driver!\n";
static const char g_system_message[] = "NXU System volume\n";
static const char g_write_message[] = "NXU writable ext4 foundation\n";
static const char g_persist_message[] = "NXU persistent ext4 marker\n";
static const char g_tail_message[] = "tail-after-sparse-growth";
static const char g_journal_message[] = "NXU JBD2 committed metadata survived the crash.\n";

#define FS_TEST_CHUNK_SIZE 1024U
#define FS_TEST_CHUNKS 12U

static bool fs_test_fail(const char *what)
{
	kprintf("i386_fs_test: FAIL %s\n", what);
	return false;
}

/* Read a whole file and require it to be exactly `expected`. */
static bool fs_test_read_exact(filedesc_t filedesc, const char *path, const char *expected, uint64_t size)
{
	uint32_t descriptor;

	if (vfs_open(filedesc, path, VFS_OPEN_READ, &descriptor) != VFS_STATUS_OK) return false;

	char buffer[128];
	uint64_t read_size = 0ULL;

	if (size >= sizeof(buffer)) {
		(void)vfs_close(filedesc, descriptor);
		return false;
	}

	memset(buffer, 0, sizeof(buffer));

	bool ok = vfs_read(filedesc, descriptor, buffer, sizeof(buffer), &read_size) == VFS_STATUS_OK &&
		read_size == size &&
		memcmp(buffer, expected, (size_t)size) == 0;

	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) ok = false;

	return ok;
}

static bool fs_test_write_file(filedesc_t filedesc, const char *path, uint32_t flags, const char *data, uint64_t size)
{
	uint32_t descriptor;

	if (vfs_open(filedesc, path, flags, &descriptor) != VFS_STATUS_OK) return false;

	uint64_t written = 0ULL;
	bool ok = vfs_write(filedesc, descriptor, data, size, &written) == VFS_STATUS_OK && written == size;

	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) ok = false;

	return ok;
}

static bool fs_test_write(void)
{
	filedesc_t filedesc = &proc_kernel()->p_fd;
	uint32_t descriptor;
	uint64_t written;

	if (!fs_test_read_exact(filedesc, "/disk/hello.txt", g_hello_message, sizeof(g_hello_message) - 1ULL)) {
		return fs_test_fail("read of /disk/hello.txt");
	}

	if (!fs_test_read_exact(filedesc, "/disk/System/README.txt", g_system_message, sizeof(g_system_message) - 1ULL)) {
		return fs_test_fail("read of /disk/System/README.txt");
	}

	vfs_status_t status = vfs_mkdir("/disk/NXU");

	if (status != VFS_STATUS_OK && status != VFS_STATUS_EXISTS) return fs_test_fail("mkdir /disk/NXU");

	/* Twelve 1 KiB chunks of a position-dependent pattern: crosses a page and grows the extent. */
	if (vfs_open(filedesc, "/disk/NXU/write-test.bin", VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, &descriptor) != VFS_STATUS_OK) {
		return fs_test_fail("create write-test.bin");
	}

	uint8_t block[FS_TEST_CHUNK_SIZE];

	for (uint32_t chunk = 0U; chunk < FS_TEST_CHUNKS; chunk++) {
		for (uint32_t index = 0U; index < sizeof(block); index++) block[index] = (uint8_t)(chunk ^ index);

		status = vfs_write(filedesc, descriptor, block, sizeof(block), &written);

		if (status != VFS_STATUS_OK || written != sizeof(block)) {
			(void)vfs_close(filedesc, descriptor);
			return fs_test_fail("append to write-test.bin");
		}
	}

	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return fs_test_fail("close write-test.bin");

	/* Shrink to 5000 bytes, then extend to 9000: the new range must read back as zeros. */
	vnode_t vnode;

	if (vfs_lookup("/disk/NXU/write-test.bin", &vnode) != VFS_STATUS_OK) return fs_test_fail("lookup write-test.bin");

	status = vnode_truncate(vnode, 5000ULL);
	if (status == VFS_STATUS_OK) status = vnode_truncate(vnode, 9000ULL);
	vnode_rele(vnode);

	if (status != VFS_STATUS_OK) return fs_test_fail("truncate write-test.bin");

	if (vfs_open(filedesc, "/disk/NXU/write-test.bin", VFS_OPEN_READ | VFS_OPEN_WRITE, &descriptor) != VFS_STATUS_OK) {
		return fs_test_fail("reopen write-test.bin");
	}

	uint8_t probe[128];
	uint64_t read_size = 0ULL;

	memset(probe, 0xA5, sizeof(probe));

	bool ok = vfs_seek(filedesc, descriptor, 5000ULL) == VFS_STATUS_OK &&
		vfs_read(filedesc, descriptor, probe, sizeof(probe), &read_size) == VFS_STATUS_OK &&
		read_size == sizeof(probe);

	for (uint32_t index = 0U; ok && index < sizeof(probe); index++) {
		if (probe[index] != 0U) ok = false;
	}

	if (!ok) {
		(void)vfs_close(filedesc, descriptor);
		return fs_test_fail("truncate-extended range is not zero");
	}

	/* A write past the current end leaves a sparse gap and extends the file to 8192 + len. */
	ok = vfs_seek(filedesc, descriptor, 8192ULL) == VFS_STATUS_OK &&
		vfs_write(filedesc, descriptor, g_tail_message, sizeof(g_tail_message) - 1ULL, &written) == VFS_STATUS_OK &&
		written == sizeof(g_tail_message) - 1ULL;

	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK || !ok) return fs_test_fail("write past end of file");

	/* Create then unlink: the name must be gone and its blocks released. */
	if (!fs_test_write_file(filedesc, "/disk/NXU/delete-me.txt", VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, g_write_message, sizeof(g_write_message) - 1ULL)) {
		return fs_test_fail("create delete-me.txt");
	}

	if (vfs_unlink("/disk/NXU/delete-me.txt") != VFS_STATUS_OK) return fs_test_fail("unlink delete-me.txt");

	status = vfs_lookup("/disk/NXU/delete-me.txt", &vnode);

	if (status != VFS_STATUS_NOT_FOUND) {
		if (status == VFS_STATUS_OK) vnode_rele(vnode);
		return fs_test_fail("unlinked file is still visible");
	}

	/* Persistence marker: created on the first boot, verified on every later one. */
	status = vfs_lookup("/disk/NXU/persistent.txt", &vnode);

	if (status == VFS_STATUS_OK) {
		vnode_rele(vnode);

		if (!fs_test_read_exact(filedesc, "/disk/NXU/persistent.txt", g_persist_message, sizeof(g_persist_message) - 1ULL)) {
			return fs_test_fail("persistence marker contents");
		}

		kputln("i386_fs_test: persistence marker recovered from previous boot");
	} else if (status == VFS_STATUS_NOT_FOUND) {
		if (!fs_test_write_file(filedesc, "/disk/NXU/persistent.txt", VFS_OPEN_WRITE | VFS_OPEN_CREATE, g_persist_message, sizeof(g_persist_message) - 1ULL)) {
			return fs_test_fail("create persistence marker");
		}

		kputln("i386_fs_test: persistence marker created");
	} else {
		return fs_test_fail("lookup of persistence marker");
	}

	if (vfs_sync_all() != VFS_STATUS_OK) return fs_test_fail("vfs_sync_all");

	if (filedesc->fd_open_count != 0U) return fs_test_fail("descriptors left open");

	kputln("i386_fs_test: write test passed");
	return true;
}

static bool fs_test_journal_crash(void)
{
	filedesc_t filedesc = &proc_kernel()->p_fd;
	uint32_t descriptor;

	vfs_status_t status = vfs_mkdir("/disk/NXU");

	if (status != VFS_STATUS_OK && status != VFS_STATUS_EXISTS) return fs_test_fail("mkdir /disk/NXU");

	/* An empty file first, so the crashing write is the only metadata change left to commit. */
	if (vfs_open(filedesc, "/disk/NXU/journal-recovery.txt", VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, &descriptor) != VFS_STATUS_OK) {
		return fs_test_fail("prepare journal-recovery.txt");
	}

	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return fs_test_fail("close journal-recovery.txt");

	if (!ext4_debug_arm_journal_crash()) return fs_test_fail("arm the journal crash point");

	if (vfs_open(filedesc, "/disk/NXU/journal-recovery.txt", VFS_OPEN_WRITE, &descriptor) != VFS_STATUS_OK) {
		return fs_test_fail("reopen journal-recovery.txt");
	}

	uint64_t written = 0ULL;

	status = vfs_write(filedesc, descriptor, g_journal_message, sizeof(g_journal_message) - 1ULL, &written);
	(void)vfs_close(filedesc, descriptor);

	if (status != VFS_STATUS_IO_ERROR) return fs_test_fail("crash point did not stop the checkpoint");

	if (!ext4_debug_journal_crash_reached()) return fs_test_fail("durable crash point was not reached");

	kputln("i386_fs_test: journal transaction committed, checkpoint intentionally skipped");
	kputln("i386_fs_test: simulating power loss");

	/* Power off with the transaction only in the journal: no unmount, no sync. */
	i386_shutdown(0x00U);
}

static bool fs_test_journal_verify(void)
{
	filedesc_t filedesc = &proc_kernel()->p_fd;

	if (!fs_test_read_exact(filedesc, "/disk/NXU/journal-recovery.txt", g_journal_message, sizeof(g_journal_message) - 1ULL)) {
		return fs_test_fail("the committed transaction was not recovered by the mount");
	}

	kputln("i386_fs_test: committed crash-test transaction recovered");
	return true;
}

bool i386_fs_test_run(const char *mode)
{
	if (strcmp(mode, "write") == 0) return fs_test_write();
	if (strcmp(mode, "journal-crash") == 0) return fs_test_journal_crash();
	if (strcmp(mode, "journal-verify") == 0) return fs_test_journal_verify();

	kprintf("i386_fs_test: unknown fs-test \"%s\"\n", mode);
	return false;
}
