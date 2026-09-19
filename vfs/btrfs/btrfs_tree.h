/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_tree.h
 *
 * B-tree access: verified tree blocks, the block cache, and the search /
 * iteration primitives every higher layer is built from.
 *
 * A search leaves a btrfs_path_t: one pinned block and one slot per level,
 * root to leaf. Iteration (btrfs_path_next/prev) walks that path, moving to a
 * neighbouring leaf through the parent slots, so a scan of a directory or of
 * the extents of a file costs one descent plus one block read per leaf. A path
 * pins its blocks in the cache (refs) until btrfs_path_release().
 *
 * Nothing here trusts the disk. A block is accepted only if its checksum, its
 * own address, its fsid, its level, its generation, its owner and the order of
 * its keys and the placement of its items all check out (see btrfs_block_get);
 * accessors re-check offsets before handing out pointers. Because a child's
 * level must be exactly one below its parent's and its first key must equal
 * the parent's key, and because iteration only ever moves to strictly larger
 * (or smaller) keys, a corrupt image cannot make a walk read outside a block,
 * recurse without bound, or loop forever.
 */

#ifndef NXU_VFS_BTRFS_BTRFS_TREE_H
#define NXU_VFS_BTRFS_BTRFS_TREE_H

#include "btrfs_fs.h"

typedef struct {
	btrfs_block_t *blocks[BTRFS_MAX_LEVEL];   /* indexed by level, 0 is the leaf */
	uint32_t slots[BTRFS_MAX_LEVEL];
	uint64_t tree_id;
	uint8_t root_level;
	bool valid;
} btrfs_path_t;

/* ---- cache and blocks ---------------------------------------------------------- */

void btrfs_cache_init(btrfs_fs_t *fs);
void btrfs_cache_release(btrfs_fs_t *fs);

/*
 * Return a verified block, from the cache or the device (trying the other copy
 * of a DUP chunk when the first fails verification). expected_level and
 * expected_generation come from the parent (generation 0 skips that check);
 * first_key, if non-NULL, must equal the block's first key; is_root permits an
 * empty leaf. The block is pinned: release it with btrfs_block_put().
 */
btrfs_status_t btrfs_block_get(btrfs_fs_t *fs, uint64_t bytenr, uint8_t expected_level, uint64_t expected_generation, uint64_t tree_id, const btrfs_key_t *first_key, bool is_root, btrfs_block_t **out);
void btrfs_block_put(btrfs_fs_t *fs, btrfs_block_t *block);

/* Item accessors. All are bounds-safe for any slot below nritems. */
bool btrfs_leaf_item(const btrfs_block_t *leaf, uint32_t slot, btrfs_key_t *key, const uint8_t **data, uint32_t *size);
bool btrfs_node_slot(const btrfs_block_t *node, uint32_t slot, btrfs_key_t *key, uint64_t *blockptr, uint64_t *generation);

/* ---- paths ---------------------------------------------------------------------------- */

void btrfs_path_init(btrfs_path_t *path);
void btrfs_path_release(btrfs_fs_t *fs, btrfs_path_t *path);

/*
 * Descend from tree's root to the leaf position of key: the first item whose
 * key is >= key (slot may equal nritems if every item in the leaf is smaller;
 * use the _ge/_le forms to get an actual item). *exact (optional) tells whether
 * that item's key equals key.
 */
btrfs_status_t btrfs_tree_search(btrfs_fs_t *fs, const btrfs_tree_t *tree, const btrfs_key_t *key, btrfs_path_t *path, bool *exact);

/* First item >= key. BTRFS_ERR_END if there is none. */
btrfs_status_t btrfs_tree_search_ge(btrfs_fs_t *fs, const btrfs_tree_t *tree, const btrfs_key_t *key, btrfs_path_t *path, bool *exact);

/* Last item <= key. BTRFS_ERR_END if there is none. */
btrfs_status_t btrfs_tree_search_le(btrfs_fs_t *fs, const btrfs_tree_t *tree, const btrfs_key_t *key, btrfs_path_t *path, bool *exact);

/* The item with exactly this key, else BTRFS_ERR_NOT_FOUND. */
btrfs_status_t btrfs_tree_lookup(btrfs_fs_t *fs, const btrfs_tree_t *tree, const btrfs_key_t *key, btrfs_path_t *path);

/* Move to the next / previous item, across leaves. BTRFS_ERR_END past either end. */
btrfs_status_t btrfs_path_next(btrfs_fs_t *fs, btrfs_path_t *path);
btrfs_status_t btrfs_path_prev(btrfs_fs_t *fs, btrfs_path_t *path);

/* The item under the path. False if the path is not on an item. data is valid until release. */
bool btrfs_path_item(const btrfs_path_t *path, btrfs_key_t *key, const uint8_t **data, uint32_t *size);

#endif
