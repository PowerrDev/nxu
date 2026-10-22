/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/startup.c
 *
 * Runs automatic i386 startup validation before normal kernel operation.
 */
/* Automatic startup validation. Destructive storage tests remain explicit. */

#include <kern/i386/startup.h>
#include <kern/i386/boot_info.h>
#include <kern/console/console.h>
#include <kern/process/proc.h>
#include <kern/tests/vm_shm_test.h>
#include <kern/tests/statistics_test.h>
#include <drivers/block/block_device.h>
#include <vfs/vfs.h>
#include <vfs/vnode.h>
#include <crc32c.h>
#include <string.h>

bool
i386_startup_tests_enabled(void)
{
	char selected[32];

	/* An isolated phase test has its own assertions and fault injections. */
	return !i386_boot_arg("test", selected, sizeof(selected)) || strcmp(selected, "desktop") == 0;
}

bool
i386_startup_core_test(void)
{
	if (crc32c(~0U, "123456789", 9) != 0x1CF96D7CU) {
		kputln("NXU_CRC32C_Checksum: self-test failed");
		return false;
	}

	kputln("NXU_CRC32C_Checksum: self-test passed");
	if (!shm_registry_self_test() || !statistics_self_test()) return false;

	kputln("i386_startup: core self-tests passed");
	return true;
}

bool
i386_startup_storage_test(void)
{
	block_device_t device = block_device_first();
	uint8_t sector[BLOCK_SECTOR_SIZE];

	if (!device || !block_device_read(device, 0, 1, sector)) {
		kputln("VirtIOBlockFamily: sector read test failed");
		return false;
	}

	kputln("VirtIOBlockFamily: sector read test passed");

	vnode_t root;

	if (vfs_lookup("/Volumes/mainOS", &root) != VFS_STATUS_OK) return false;

	bool ok = root->v_type == VNODE_TYPE_DIRECTORY;

	vnode_rele(root);
	if (!ok) return false;

	uint32_t fd;

	if (vfs_open(&proc_kernel()->p_fd, "/Volumes/mainOS", VFS_OPEN_READ, &fd) != VFS_STATUS_OK) {
		return false;
	}

	vfs_dirent_t entry;
	vfs_status_t status = vfs_readdir(&proc_kernel()->p_fd, fd, &entry);
	vfs_status_t closed = vfs_close(&proc_kernel()->p_fd, fd);

	if ((status != VFS_STATUS_OK && status != VFS_STATUS_END_OF_DIRECTORY) || closed != VFS_STATUS_OK) {
		return false;
	}

	kputln("i386_startup: storage self-tests passed (sector read and ext4 directory traversal)");
	return true;
}
