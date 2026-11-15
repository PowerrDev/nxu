/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/boot/startup_disk.c
 *
 * Honours the Startup Disk choice made in System Settings. patchd writes the
 * choice to NSW_STARTUP_DISK on the recovery volume ("mainOS\n" or
 * "Recovery\n"), because NVRAM is read-only under QEMU and the recovery
 * volume is the one place both environments can always read. A Recovery
 * choice is one-shot: it is put back to mainOS, durably, before recovery
 * starts, so nobody can be stuck in recovery by it.
 */
#include <kern/boot/startup_disk.h>
#include <frameworks/SoftwareUpdate.framework/include/nsw.h>
#include <kern/boot/boot_mode.h>
#include <kern/boot/recovery_volume.h>
#include <kern/console/console.h>
#include <kern/process/proc.h>
#include <libk/string.h>
#include <vfs/vfs.h>

#define STARTUP_DISK_MAINOS "mainOS\n"
#define STARTUP_DISK_RECOVERY "Recovery\n"

static bool
startup_disk_read(char *bytes, uint32_t capacity, uint32_t *size)
{
	vnode_t vnode;

	*size = 0;
	if (vfs_lookup(NSW_STARTUP_DISK, &vnode) != VFS_STATUS_OK) return false;

	bool success = vnode->v_type == VNODE_TYPE_REGULAR && vnode->v_size <= capacity;
	uint32_t length = success ? (uint32_t)vnode->v_size : 0;
	uint32_t complete = 0;

	while (success && complete < length) {
		uint64_t read = 0;
		success = vnode_read(vnode, complete, bytes + complete, length - complete, &read) == VFS_STATUS_OK &&
				  read > 0 && read <= length - complete;
		if (success) complete += (uint32_t)read;
	}

	vnode_rele(vnode);
	if (success) *size = length;
	return success;
}

static bool
startup_disk_write(const char *path, const char *text)
{
	filedesc_t files = &proc_kernel()->p_fd;
	uint32_t descriptor;
	uint32_t size = strlen(text);

	if (vfs_open(files, path, VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, &descriptor) != VFS_STATUS_OK) {
		return false;
	}

	uint64_t written = 0;
	bool success = vfs_write(files, descriptor, text, size, &written) == VFS_STATUS_OK && written == size;

	return vfs_close(files, descriptor) == VFS_STATUS_OK && success;
}

static bool
startup_disk_reset(void)
{
	/* Back to mainOS on the recovery volume, then the mirror System Settings reads. */
	bool stored = startup_disk_write(NSW_STARTUP_DISK, STARTUP_DISK_MAINOS) && vfs_sync_all() == VFS_STATUS_OK;

	if (!startup_disk_write(NSW_STARTUP_DISK_MIRROR, STARTUP_DISK_MAINOS)) {
		kprintf("could not update %s (patchd rewrites it at its next start)\n", NSW_STARTUP_DISK_MIRROR);
	}

	(void)vfs_sync_all();
	return stored;
}

void
boot_startup_disk_init(void)
{
	kprintf("initializing the startup disk selection\n");

	char choice[32];
	uint32_t size = 0;

	if (!boot_recovery_volume_available()) {
		kprintf("no recovery volume: the startup disk is mainOS\n");
	} else if (!startup_disk_read(choice, sizeof(choice), &size)) {
		kprintf("no startup disk chosen (%s is absent): starting mainOS\n", NSW_STARTUP_DISK);
	} else if (size == strlen(STARTUP_DISK_MAINOS) && memcmp(choice, STARTUP_DISK_MAINOS, size) == 0) {
		kprintf("startup disk is mainOS\n");
	} else if (size == strlen(STARTUP_DISK_RECOVERY) && memcmp(choice, STARTUP_DISK_RECOVERY, size) == 0) {
		/* Reset first: a choice that cannot be undone is not honoured, or every boot would land in recovery. */
		if (startup_disk_reset()) {
			kprintf("startup disk is Recovery: starting triageOS for this boot\n");
			kprintf("startup disk reset to mainOS (Recovery is a one-shot choice)\n");
			boot_mode_force_recovery();
		} else {
			kprintf("startup disk is Recovery, but the choice could not be reset to mainOS; starting mainOS\n");
		}
	} else {
		kprintf("startup disk choice in %s is malformed; using mainOS\n", NSW_STARTUP_DISK);
		(void)startup_disk_reset();
	}

	kprintf("initialized the startup disk selection successfully (%s)\n", boot_mode_name());
}
