/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_root.h
 *
 * The root tree: where every other tree (the subvolumes, the extent, dev,
 * csum, ... trees) is found, and the subvolume relationships.
 *
 * ROOT_ITEM (id, ROOT_ITEM, offset)   the root node and generation of tree
 *                                     `id`; the item with the largest offset
 *                                     is current (snapshots used to encode
 *                                     their creation transid there).
 * ROOT_REF (parent, ROOT_REF, child)  "child is named <name> in directory
 *                                     <dirid> of <parent>", plus the DIR_INDEX
 *                                     of that directory entry.
 * ROOT_BACKREF (child, ROOT_BACKREF, parent)  the same, keyed from the child.
 * DIR_ITEM in tree 6 named "default"  the default subvolume.
 *
 * FS_TREE (5) is always the top-level subvolume; user subvolumes and snapshots
 * have ids >= 256.
 */

#ifndef NXU_VFS_BTRFS_BTRFS_ROOT_H
#define NXU_VFS_BTRFS_BTRFS_ROOT_H

#include "btrfs_fs.h"

/* An opened subvolume (or any tree named in the root tree). */
typedef struct {
	uint64_t id;
	btrfs_root_item_t item;
	btrfs_tree_t tree;         /* ready to pass to the search functions */
	bool read_only;            /* ROOT_SUBVOL_RDONLY: a read-only snapshot */
} btrfs_subvol_t;

/* A subvolume relationship read from ROOT_REF / ROOT_BACKREF. */
typedef struct {
	uint64_t parent_id;        /* tree containing the subvolume point */
	uint64_t child_id;
	uint64_t dirid;            /* directory inode in parent_id holding the point */
	uint64_t sequence;         /* its DIR_INDEX */
	uint16_t name_len;
	char name[BTRFS_NAME_LEN + 1U];
} btrfs_root_ref_info_t;

/* Look a tree up by id. BTRFS_ERR_NOT_FOUND if the root tree has no such tree. */
btrfs_status_t btrfs_root_lookup(btrfs_fs_t *fs, uint64_t id, btrfs_subvol_t *out);

/* The id the "default" directory item of the root tree names (5 if absent). */
btrfs_status_t btrfs_root_default_id(btrfs_fs_t *fs, uint64_t *id);

/* Fill fs->default_subvol and fs->mount_subvol and check the latter exists. */
btrfs_status_t btrfs_root_select_default(btrfs_fs_t *fs);

/*
 * Resolve a directory entry of tree `parent_id` (directory inode `dir_ino`) that
 * names subvolume `child_id` (its location key has type ROOT_ITEM).
 *
 * BTRFS_OK: the entry is a real subvolume point; *out is the opened subvolume.
 * BTRFS_ERR_NOT_FOUND: the entry is a placeholder. A snapshot copies the
 * directory entry of every subvolume nested in its source but not the
 * subvolume itself (that one still hangs below the *source*, in ROOT_REF terms),
 * so Linux shows an empty directory there (inode 2, mode 0755, size 0); so
 * should we. The same answer covers a subvolume that is being deleted.
 */
btrfs_status_t btrfs_root_resolve_point(btrfs_fs_t *fs, uint64_t parent_id, uint64_t dir_ino, const uint8_t *name, size_t name_len, uint64_t child_id, btrfs_subvol_t *out);

/* Where a subvolume hangs: its ROOT_BACKREF (NOT_FOUND for the top level or a rootless tree). */
btrfs_status_t btrfs_root_backref(btrfs_fs_t *fs, uint64_t child_id, btrfs_root_ref_info_t *out);

/*
 * Iterate the subvolumes that hang directly under parent_id (ROOT_REF items).
 * *cursor starts at 0 and is advanced; BTRFS_ERR_END when there are no more.
 */
btrfs_status_t btrfs_root_next_child(btrfs_fs_t *fs, uint64_t parent_id, uint64_t *cursor, btrfs_root_ref_info_t *out);

/* Iterate every subvolume id (ROOT_ITEMs >= 256, plus 5). *cursor starts at 0. */
btrfs_status_t btrfs_root_next_subvol(btrfs_fs_t *fs, uint64_t *cursor, uint64_t *id);

#endif
