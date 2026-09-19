/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_selftest.h
 *
 * The in-kernel Btrfs test, shared by arm64 (kern/kern_init.c) and i386
 * (mach/i386/userland_init.c), selected by the boot argument
 *
 *     btrfs-test=<spec>[,<spec>...]
 *
 * The Nth spec is run against the Nth block device after the first (device 0
 * is the root disk). A spec is either
 *
 *   NAME[@SUBVOLID][+verify]    mount the fixture read-only (verify: with data
 *                               checksums), walk it and compare with the
 *                               expected listing generated from Linux's
 *                               manifest (btrfs_selftest_data.h): path, type,
 *                               mode, inode, size, mtime, crc32c of contents,
 *                               symlink target; readdir cursor resume; reads at
 *                               odd offsets and past EOF; every mutating
 *                               operation must return READ_ONLY and leave the
 *                               device unwritten; then unmount
 *   !STATUS                     the image is damaged or unsupported: the mount
 *                               must fail, cleanly, with exactly that
 *                               btrfs_status_t (bad-magic, csum, corrupt,
 *                               truncated, unsupported-feature,
 *                               unsupported-csum, unsupported-profile,
 *                               log-tree), and the kernel must keep running
 *
 * Output lines start with "btrfs_selftest:". The run ends with
 * "btrfs_selftest: ALL PASSED" or "btrfs_selftest: FAILED (<n> failures)".
 */

#ifndef NXU_VFS_BTRFS_BTRFS_SELFTEST_H
#define NXU_VFS_BTRFS_BTRFS_SELFTEST_H

#include <stdbool.h>

/* Registers the "btrfs" filesystem (only here, never at boot) and runs the specs. */
bool btrfs_selftest_run(const char *spec);

#endif
