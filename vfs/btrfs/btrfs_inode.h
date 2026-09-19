/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_inode.h
 *
 * Inodes of one subvolume tree.
 *
 * Per inode `ino`, the subvolume tree holds items keyed (ino, type, offset):
 *
 *   INODE_ITEM   (ino, 1, 0)      mode, owner, size, nlink, times, rdev, flags
 *   INODE_REF    (ino, 12, parent) one or more (dir index, name) in `parent`
 *   INODE_EXTREF (ino, 13, hash)  the same when an INODE_REF item would not
 *                                 fit in a leaf (many hard links)
 *   XATTR_ITEM   (ino, 24, hash)  extended attributes (skipped by this driver)
 *   DIR_ITEM/DIR_INDEX/EXTENT_DATA  see btrfs_dir.h and btrfs_file.h
 *
 * A hard link is a second name for one inode: nlink counts the refs, and the
 * refs are the way back from an inode to its parents.
 */

#ifndef NXU_VFS_BTRFS_BTRFS_INODE_H
#define NXU_VFS_BTRFS_BTRFS_INODE_H

#include "btrfs_fs.h"

typedef struct {
	uint64_t ino;
	btrfs_inode_item_t item;
} btrfs_inode_t;

/* One name of an inode: from an INODE_REF or an INODE_EXTREF. */
typedef struct {
	uint64_t parent;           /* directory inode holding the name */
	uint64_t index;            /* its DIR_INDEX in that directory */
	uint16_t name_len;
	uint8_t name[BTRFS_NAME_LEN + 1U];
	bool extended;             /* came from an INODE_EXTREF */
} btrfs_inode_ref_t;

/* Return false to stop the enumeration early. */
typedef bool (*btrfs_inode_ref_fn)(void *context, const btrfs_inode_ref_t *ref);

/* Read and validate the INODE_ITEM of `ino` in `subvol`. */
btrfs_status_t btrfs_inode_read(btrfs_fs_t *fs, const btrfs_tree_t *subvol, uint64_t ino, btrfs_inode_t *out);

/*
 * Enumerate every name of `ino` (INODE_REF then INODE_EXTREF items). *count
 * receives the number of names visited.
 */
btrfs_status_t btrfs_inode_refs(btrfs_fs_t *fs, const btrfs_tree_t *subvol, uint64_t ino, btrfs_inode_ref_fn callback, void *context, uint32_t *count);

/* File type bits of a mode, as a BTRFS_S_IF* value, or 0 if the mode is not a known type. */
uint32_t btrfs_inode_type(uint32_t mode);

#endif
