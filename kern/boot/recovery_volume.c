/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <kern/boot/recovery_volume.h>
#include <kern/boot/boot_args.h>
#include <kern/console/console.h>
#include <drivers/block/block_device.h>
#include <drivers/block/partition.h>
#include <kern/boot/boot_mode.h>
#include <libk/string.h>
#include <vfs/vfs.h>

static block_device_t g_recovery_device;
static bool g_recovery_mounted;

block_device_t
boot_system_volume_device(void)
{
	block_device_t disk = block_device_first();
	block_layout_info_t layout;
	if (disk == 0 || !block_layout_inspect(disk, &layout)) {
		return 0;
	}
	if (layout.scheme == BLOCK_LAYOUT_RAW) {
		return disk;
	}
	if (layout.scheme != BLOCK_LAYOUT_GPT) {
		kputln("boot: system disk needs a raw filesystem or named GPT system partition");
		return 0;
	}
	block_partition_info_t system = {0}, recovery = {0};
	uint32_t systems = 0, recoveries = 0;
	for (uint32_t index = 0; index < layout.partition_count; index++) {
		block_partition_info_t info;
		if (!block_partition_get(disk, index, &info)) {
			return 0;
		}
		if (strcmp(info.name, "NXU System") == 0) {
			system = info;
			systems++;
		} else if (strcmp(info.name, "NXU Recovery") == 0) {
			recovery = info;
			recoveries++;
		}
	}
	if (systems != 1 || recoveries > 1 ||
		(recoveries == 1 && system.start_sector < recovery.start_sector + recovery.sector_count &&
		 recovery.start_sector < system.start_sector + system.sector_count)) {
		kputln("boot: missing, ambiguous or overlapping system/recovery partitions");
		return 0;
	}
	block_device_t selected = block_partition_device(disk, &system);
	if (selected == 0) {
		return 0;
	}
	if (recoveries == 1) {
		g_recovery_device = block_partition_device(disk, &recovery);
		if (g_recovery_device == 0) {
			return 0;
		}
	}
	kprintf("boot: selected GPT system volume %s\n", selected->name);
	return selected;
}

const char *
boot_recovery_volume_program(void)
{
	return boot_recovery_volume_available() ? "/Volumes/Recovery/System/Recovery/triageOS"
											: "/disk/System/Recovery/triageOS";
}

bool
boot_recovery_volume_available(void)
{
	vnode_t program;
	if (!g_recovery_mounted || vfs_lookup("/Volumes/Recovery/System/Recovery/triageOS", &program) != VFS_STATUS_OK) {
		return false;
	}
	bool available = program->v_type == VNODE_TYPE_REGULAR;
	vnode_rele(program);
	return available;
}

void
boot_mount_recovery_volume(void)
{
	char value[16];
	bool explicit = boot_arg_value("recovery.volume", value, sizeof(value));
	if (!explicit && g_recovery_device == 0) {
		return;
	}

	uint32_t index = explicit ? 0 : g_recovery_device->device_id - 1;
	if (explicit) {
		if (value[0] == 0) {
			kputln("recovery: invalid volume index");
			return;
		}
		for (uint32_t digit = 0; value[digit] != 0; digit++) {
			if (value[digit] < '0' || value[digit] > '9') {
				kputln("recovery: invalid volume index");
				return;
			}
			index = index * 10 + (uint32_t)(value[digit] - '0');
			if (index >= BLOCK_DEVICE_MAX) {
				kputln("recovery: invalid volume index");
				return;
			}
		}
	}
	block_device_t device = block_device_get(index);
	if (device == 0) {
		kputln("recovery: configured volume is unavailable");
		return;
	}
	vfs_mount_info_t info;
	for (uint32_t mount = 0; vfs_mount_get(mount, &info); mount++) {
		if (info.device_index == index) {
			kputln("recovery: configured device is already mounted");
			return;
		}
	}

	const char *paths[] = {"/Volumes", "/Volumes/Recovery"};
	for (uint32_t path = 0; path < 2; path++) {
		vfs_status_t status = vfs_mkdir(paths[path]);
		if (status != VFS_STATUS_OK && status != VFS_STATUS_EXISTS) {
			kputln("recovery: volume mountpoint creation failed");
			return;
		}
	}
	vfs_status_t status = vfs_mount("ext4", device, "/Volumes/Recovery");
	if (status != VFS_STATUS_OK) {
		kprintf("recovery: volume mount failed: %s\n", vfs_status_name(status));
		return;
	}
	g_recovery_mounted = true;
	/* No updater is running yet: clear an empty lock left by a previous boot. */
	(void)vfs_rmdir("/Volumes/Recovery/Prepared/.prepare-lock");
	kputln("recovery: mounted update volume at /Volumes/Recovery");
}
