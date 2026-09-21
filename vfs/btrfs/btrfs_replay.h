/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_replay.h
 *
 * Read-only replay of the log tree.
 *
 * fsync() does not commit a transaction: it writes what changed into a log
 * tree and points the superblock at it (super.log_root). If the machine stops
 * before the next commit, Linux replays the log at the next mount and the
 * result is what the file system contains. This driver never writes, so it
 * cannot replay the log into the trees; instead it reads the log tree into
 * memory when it opens the volume and layers it over the real trees, so that
 * every read shows what Linux would show after replay:
 *
 *   inode items   a logged INODE_ITEM replaces the item in the fs tree (and
 *                 inodes that exist only in the log are found)
 *   directories   DIR_INDEX items of the log and the names of logged
 *                 INODE_REF items add or replace entries by index; entries in a
 *                 logged range (DIR_LOG_INDEX) that the log does not repeat, and
 *                 names a logged INODE_REF no longer lists, are removed; the
 *                 size of a changed directory follows the names added or removed
 *   file extents  logged EXTENT_DATA items replace what they overlap (the old
 *                 extents are trimmed), explicit hole extents included
 *   checksums     the EXTENT_CSUM items of the log verify the logged data
 *   inode refs    a logged INODE_REF item replaces the fs item of the same key
 *
 * The volume itself is never touched. Logs whose content this layer does not
 * understand (unknown item types, extended inode refs, subvolume entries) make
 * the mount fail with BTRFS_ERR_LOG_TREE, as before; ignore_log_tree still
 * skips the log altogether and shows the state of the last commit.
 */

#ifndef NXU_VFS_BTRFS_BTRFS_REPLAY_H
#define NXU_VFS_BTRFS_BTRFS_REPLAY_H

#include "btrfs_fs.h"

#define BTRFS_REPLAY_ITEMS_MAX 262144U
#define BTRFS_REPLAY_BYTES_MAX (16U * 1024U * 1024U)
#define BTRFS_REPLAY_DIRENTS_MAX 32768U

typedef struct {
	btrfs_key_t key;
	uint32_t offset;           /* of the item's bytes in btrfs_replay_t.data */
	uint32_t size;
} btrfs_log_item_t;

/* One directory entry the log adds: from a DIR_INDEX item or a logged INODE_REF. */
typedef struct {
	uint64_t dir;
	uint64_t index;
	uint64_t ino;
	uint32_t name_offset;      /* into data */
	uint16_t name_len;
	uint8_t type;              /* BTRFS_FT_* */
} btrfs_log_dirent_t;

typedef struct {
	uint64_t dir;
	uint64_t index;
} btrfs_log_gone_t;

struct btrfs_replay {
	btrfs_replay_t *next;
	uint64_t subvol;
	btrfs_log_item_t *items;   /* sorted by key */
	uint32_t count;
	uint32_t capacity;
	uint8_t *data;
	size_t data_size;
	size_t data_capacity;
	btrfs_log_dirent_t *dirents;   /* sorted by (dir, index) */
	uint32_t dirent_count;
	btrfs_log_gone_t *gone;        /* entries of the fs tree that the log removes by name */
	uint32_t gone_count;
	uint32_t csum_first;           /* index of the first EXTENT_CSUM item (count: none) */
};

/*
 * Read the log tree the superblock points to. Called once by btrfs_fs_open()
 * after the root tree is known; a no-op without a log or with ignore_log_tree.
 */
btrfs_status_t btrfs_replay_open(btrfs_fs_t *fs);
void btrfs_replay_close(btrfs_fs_t *fs);

/* The log of a subvolume, or NULL. */
const btrfs_replay_t *btrfs_replay_find(const btrfs_fs_t *fs, uint64_t subvol);

/* The item with exactly this key, or NULL; its bytes via btrfs_replay_bytes(). */
const btrfs_log_item_t *btrfs_replay_item(const btrfs_replay_t *replay, const btrfs_key_t *key);

/* Index of the first item with a key >= key (replay->count if none). */
uint32_t btrfs_replay_lower_bound(const btrfs_replay_t *replay, const btrfs_key_t *key);

static inline const uint8_t *btrfs_replay_bytes(const btrfs_replay_t *replay, const btrfs_log_item_t *item)
{
	return replay->data + item->offset;
}

/* Does the log say anything about this directory's entries? */
bool btrfs_replay_dir_touched(const btrfs_replay_t *replay, uint64_t dir);

/* The logged entry with the smallest index >= index in `dir`, or NULL. */
const btrfs_log_dirent_t *btrfs_replay_dirent_ge(const btrfs_replay_t *replay, uint64_t dir, uint64_t index);

/* Does the log remove the fs tree's entry (dir, index)? (Not consulted for an index the log itself lists.) */
bool btrfs_replay_dir_removes(const btrfs_replay_t *replay, uint64_t dir, uint64_t index);

/* The logged checksum item covering a data address, or NULL; *start is its first address. */
const uint8_t *btrfs_replay_csum(const btrfs_fs_t *fs, uint64_t address, uint64_t *start, uint64_t *end);

#endif
