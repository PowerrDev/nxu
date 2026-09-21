/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs.h
 *
 * The read-only Btrfs filesystem as the rest of the kernel sees it: register
 * the "btrfs" type with the VFS, mount a block device with
 * vfs_mount("btrfs", device, path), and inspect the result.
 *
 * Every operation that would modify the volume (create, write, truncate,
 * unlink, ...) returns VFS_STATUS_READ_ONLY.
 */

#ifndef NXU_VFS_BTRFS_BTRFS_H
#define NXU_VFS_BTRFS_BTRFS_H

#include <vfs/btrfs/btrfs_format.h>

#include <drivers/block/block_device.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * btrfs_register:
 *
 * Publish the Btrfs filesystem implementation to VFS. Registration creates no
 * mount and performs no block I/O.
 */
bool btrfs_register(void);

/*
 * Options for the NEXT vfs_mount("btrfs", ...): vfs_mount() has no options
 * argument, so they are handed over out of band and consumed by that mount
 * (whether it succeeds or not). NULL clears them.
 *
 *   subvol_id           mount this subvolume instead of the default (0: default)
 *   verify_data         check file data against the checksum tree (the
 *                       default; kept as an explicit spelling)
 *   ignore_log_tree     mount despite an unreplayed log tree
 *   noverify            do not check file data against the checksum tree
 *                       (metadata is always checked). Together with verify_data
 *                       the mount is refused as contradictory.
 *   extra_devices       the other devices of a multi-device filesystem (the
 *                       device given to vfs_mount is the first); every device
 *                       the filesystem lists must be supplied, in any order,
 *                       else the mount fails with BTRFS_ERR_MISSING_DEVICE
 */
#define BTRFS_MOUNT_EXTRA_DEVICES 3U

typedef struct {
	uint64_t subvol_id;
	bool verify_data;
	bool ignore_log_tree;
	bool noverify;
	block_device_t extra_devices[BTRFS_MOUNT_EXTRA_DEVICES];
	uint32_t extra_count;
} btrfs_mount_options_t;

void btrfs_set_next_mount_options(const btrfs_mount_options_t *options);

/*
 * The precise reason the most recent mount attempt failed (or BTRFS_OK). The
 * VFS status collapses several causes; this keeps the distinction, for
 * example "unsupported checksum type" against "checksum mismatch".
 */
btrfs_status_t btrfs_last_mount_status(void);
const char *btrfs_last_mount_status_name(void);

uint32_t btrfs_mount_count(void);

/* Print, for every mounted volume, the superblock facts, cache and I/O counters. */
void btrfs_dump(void);

#endif
