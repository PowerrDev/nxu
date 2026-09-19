/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_list.h
 *
 * Mount an arbitrary Btrfs disk read-only and look at it: print its tree like
 * `ls -lR` and/or one file's contents to the kernel console. Unlike
 * btrfs_selftest.h, which grades a mount against expected listings and so only
 * knows the fixtures, this works on any image, which is what a `run` style
 * boot of a Btrfs disk needs (see tools/btrfs/run_i386.sh, make
 * run-i386-btrfs). Arch-neutral: only the VFS and Btrfs public interfaces.
 *
 * Output: the mount's own description lines, then one line per entry
 *
 *     <type><mode> <nlink> <size> <path>[ -> <symlink target>]
 *
 * (type is d - l c b p s, mode is the nine rwx characters), then a summary.
 */

#ifndef NXU_VFS_BTRFS_BTRFS_LIST_H
#define NXU_VFS_BTRFS_BTRFS_LIST_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t device_index;    /* block device to mount, 0 is the first */
	uint64_t subvolume;       /* 0: the default subvolume */
	bool verify;              /* verify data checksums while reading */
	bool list;                /* print the whole tree */
	uint32_t max_entries;     /* stop printing after this many entries (0: 512) */
	const char *cat_path;     /* print this file, path relative to the mount root; 0: none */
} btrfs_list_request_t;

/*
 * Registers the "btrfs" filesystem (never at boot), mounts the device
 * read-only, does what the request asks, and unmounts again (which also proves
 * no vnode reference leaked). Returns true if the mount and every requested
 * step succeeded.
 */
bool btrfs_list_run(const btrfs_list_request_t *request);

#endif
