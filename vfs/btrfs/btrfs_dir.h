/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_dir.h
 *
 * Directories. Btrfs stores every directory entry twice, in a directory
 * inode `dir`:
 *
 *   DIR_ITEM  (dir, 84, crc32c(~1, name))  for lookup by name; entries whose
 *                                          names hash alike are packed into
 *                                          one item
 *   DIR_INDEX (dir, 96, index)             for readdir in creation order;
 *                                          index counts up from 2
 *
 * Each entry names a `location` key: (inode, INODE_ITEM, 0) for a file or
 * directory in the same subvolume, or (subvol id, ROOT_ITEM, -1) for a
 * subvolume point, where the walk continues in another tree. "." and ".."
 * are not stored; the VFS glue synthesises them.
 */

#ifndef NXU_VFS_BTRFS_BTRFS_DIR_H
#define NXU_VFS_BTRFS_BTRFS_DIR_H

#include "btrfs_fs.h"

typedef struct {
	btrfs_key_t location;
	uint64_t transid;
	uint64_t index;            /* DIR_INDEX; 0 for an entry found by name */
	uint8_t type;              /* BTRFS_FT_* */
	uint16_t name_len;
	uint8_t name[BTRFS_NAME_LEN + 1U];   /* NUL terminated */
} btrfs_dirent_t;

/* True if the entry leads into another subvolume. */
static inline bool btrfs_dirent_is_subvol(const btrfs_dirent_t *entry)
{
	return entry->location.type == BTRFS_ROOT_ITEM_KEY;
}

/* Find `name` (length bytes) in directory `dir`. BTRFS_ERR_NOT_FOUND if absent. */
btrfs_status_t btrfs_dir_lookup(btrfs_fs_t *fs, const btrfs_tree_t *subvol, uint64_t dir, const uint8_t *name, size_t length, btrfs_dirent_t *out);

/*
 * The first entry with DIR_INDEX >= *cursor. On success *cursor becomes the
 * entry's index + 1, so passing it back resumes exactly after this entry.
 * BTRFS_ERR_END when the directory is exhausted. Start with *cursor = 0.
 */
btrfs_status_t btrfs_dir_next(btrfs_fs_t *fs, const btrfs_tree_t *subvol, uint64_t dir, uint64_t *cursor, btrfs_dirent_t *out);

#endif
