/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_inode.c
 *
 * INODE_ITEM reading and INODE_REF / INODE_EXTREF enumeration.
 */

#include "btrfs_inode.h"
#include "btrfs_dir.h"
#include "btrfs_replay.h"
#include "btrfs_tree.h"

#include <string.h>

uint32_t btrfs_inode_type(uint32_t mode)
{
	uint32_t type = mode & BTRFS_S_IFMT;

	switch (type) {
	case BTRFS_S_IFSOCK:
	case BTRFS_S_IFLNK:
	case BTRFS_S_IFREG:
	case BTRFS_S_IFBLK:
	case BTRFS_S_IFDIR:
	case BTRFS_S_IFCHR:
	case BTRFS_S_IFIFO:
		return type;
	default:
		return 0U;
	}
}

/* The committed inode item, without the log. */
static btrfs_status_t btrfs_inode_read_committed(btrfs_fs_t *fs, const btrfs_tree_t *subvol, uint64_t ino, btrfs_inode_item_t *item)
{
	btrfs_path_t path;
	btrfs_key_t key = btrfs_key_make(ino, BTRFS_INODE_ITEM_KEY, 0ULL);
	const uint8_t *data;
	uint32_t size;

	btrfs_path_init(&path);
	btrfs_status_t status = btrfs_tree_lookup(fs, subvol, &key, &path);
	if (status != BTRFS_OK) return status;

	bool ok = btrfs_path_item(&path, 0, &data, &size) && btrfs_inode_item_parse(data, size, item);
	btrfs_path_release(fs, &path);

	return ok ? BTRFS_OK : BTRFS_ERR_CORRUPT;
}

/*
 * The size of a directory the log changed. Linux keeps the committed size of a
 * directory when it replays the logged inode item and then adds 2 * name length
 * for every entry the replay links and subtracts it for every entry it unlinks;
 * a directory that exists only in the log starts from the logged size. Diffing
 * the committed listing against the merged one gives the same numbers.
 */
static btrfs_status_t btrfs_inode_replayed_dir_size(btrfs_fs_t *fs, const btrfs_tree_t *subvol, uint64_t dir, uint64_t base, uint64_t *size)
{
	uint64_t committed_cursor = 0ULL;
	uint64_t merged_cursor = 0ULL;
	btrfs_dirent_t committed;
	btrfs_dirent_t merged;
	btrfs_status_t cs = btrfs_dir_next_committed(fs, subvol, dir, &committed_cursor, &committed);
	btrfs_status_t ms = btrfs_dir_next(fs, subvol, dir, &merged_cursor, &merged);
	uint64_t total = base;

	while (cs == BTRFS_OK || ms == BTRFS_OK) {
		if (cs != BTRFS_OK && cs != BTRFS_ERR_END) return cs;
		if (ms != BTRFS_OK && ms != BTRFS_ERR_END) return ms;

		bool take_committed = cs == BTRFS_OK && (ms != BTRFS_OK || committed.index <= merged.index);
		bool take_merged = ms == BTRFS_OK && (cs != BTRFS_OK || merged.index <= committed.index);

		if (take_committed && take_merged && committed.name_len == merged.name_len && memcmp(committed.name, merged.name, merged.name_len) == 0) {
			/* Unchanged. */
		} else {
			if (take_committed) total -= 2ULL * committed.name_len;
			if (take_merged) total += 2ULL * merged.name_len;
		}

		if (take_committed) cs = btrfs_dir_next_committed(fs, subvol, dir, &committed_cursor, &committed);
		if (take_merged) ms = btrfs_dir_next(fs, subvol, dir, &merged_cursor, &merged);
	}

	if (cs != BTRFS_ERR_END) return cs;
	if (ms != BTRFS_ERR_END) return ms;

	*size = total;
	return BTRFS_OK;
}

btrfs_status_t btrfs_inode_read(btrfs_fs_t *fs, const btrfs_tree_t *subvol, uint64_t ino, btrfs_inode_t *out)
{
	const btrfs_replay_t *replay = btrfs_replay_find(fs, subvol->objectid);
	btrfs_key_t key = btrfs_key_make(ino, BTRFS_INODE_ITEM_KEY, 0ULL);
	const btrfs_log_item_t *logged = replay != 0 ? btrfs_replay_item(replay, &key) : 0;
	btrfs_inode_item_t item;
	btrfs_status_t status;

	if (logged != 0) {
		/* The logged item is what a replay would leave. */
		if (!btrfs_inode_item_parse(btrfs_replay_bytes(replay, logged), logged->size, &item)) return BTRFS_ERR_CORRUPT;
	} else {
		status = btrfs_inode_read_committed(fs, subvol, ino, &item);
		if (status != BTRFS_OK) return status;
	}

	if (logged != 0 && btrfs_inode_type(item.mode) == BTRFS_S_IFDIR && btrfs_replay_dir_touched(replay, ino)) {
		btrfs_inode_item_t old;

		status = btrfs_inode_read_committed(fs, subvol, ino, &old);
		if (status == BTRFS_OK) item.size = old.size;
		else if (status != BTRFS_ERR_NOT_FOUND) return status;

		status = btrfs_inode_replayed_dir_size(fs, subvol, ino, item.size, &item.size);
		if (status != BTRFS_OK) return status;
	}
	if (btrfs_inode_type(item.mode) == 0U) {
		BTRFS_LOG(fs, "inode %llu has an unknown file type in mode %x", BTRFS_U64(ino), item.mode);
		return BTRFS_ERR_CORRUPT;
	}

	out->ino = ino;
	out->item = item;
	return BTRFS_OK;
}

btrfs_status_t btrfs_inode_refs(btrfs_fs_t *fs, const btrfs_tree_t *subvol, uint64_t ino, btrfs_inode_ref_fn callback, void *context, uint32_t *count)
{
	btrfs_path_t path;
	btrfs_key_t target = btrfs_key_make(ino, BTRFS_INODE_REF_KEY, 0ULL);
	uint32_t visited = 0U;
	const btrfs_replay_t *replay = btrfs_replay_find(fs, subvol->objectid);
	bool stopped = false;

	if (count != 0) *count = 0U;

	btrfs_path_init(&path);
	btrfs_status_t status = btrfs_tree_search_ge(fs, subvol, &target, &path, 0);

	while (status == BTRFS_OK) {
		btrfs_key_t key;
		const uint8_t *data;
		uint32_t size;

		if (!btrfs_path_item(&path, &key, &data, &size)) {
			status = BTRFS_ERR_CORRUPT;
			break;
		}

		if (key.objectid != ino || (key.type != BTRFS_INODE_REF_KEY && key.type != BTRFS_INODE_EXTREF_KEY)) break;

		size_t position = 0U;
		bool stop = false;

		/* The log has its own item for this parent: it replaces the committed one. */
		if (replay != 0 && key.type == BTRFS_INODE_REF_KEY) {
			btrfs_key_t logged_key = btrfs_key_make(ino, BTRFS_INODE_REF_KEY, key.offset);

			if (btrfs_replay_item(replay, &logged_key) != 0) position = size;
		}

		/* One item packs several names when they share a parent (or a hash). */
		while (position < size) {
			btrfs_inode_ref_t ref;
			size_t header;
			size_t remaining = size - position;

			if (key.type == BTRFS_INODE_REF_KEY) {
				header = 10U; /* index u64, name_len u16 */
				if (remaining < header) { status = BTRFS_ERR_CORRUPT; break; }
				ref.parent = key.offset;
				ref.index = btrfs_get_le64(data + position);
				ref.name_len = btrfs_get_le16(data + position + 8);
				ref.extended = false;
			} else {
				header = 18U; /* parent u64, index u64, name_len u16 */
				if (remaining < header) { status = BTRFS_ERR_CORRUPT; break; }
				ref.parent = btrfs_get_le64(data + position);
				ref.index = btrfs_get_le64(data + position + 8);
				ref.name_len = btrfs_get_le16(data + position + 16);
				ref.extended = true;
			}

			if (ref.name_len == 0U || ref.name_len > BTRFS_NAME_LEN || remaining - header < ref.name_len) {
				status = BTRFS_ERR_CORRUPT;
				break;
			}

			memcpy(ref.name, data + position + header, ref.name_len);
			ref.name[ref.name_len] = '\0';
			position += header + ref.name_len;
			visited++;

			if (callback != 0 && !callback(context, &ref)) {
				stop = true;
				break;
			}
		}

		if (status != BTRFS_OK || stop) {
			stopped = stop;
			break;
		}

		status = btrfs_path_next(fs, &path);
	}

	btrfs_path_release(fs, &path);
	if (status == BTRFS_ERR_END) status = BTRFS_OK;

	/* Then what the log lists (INODE_REF items only: the log holds no extended refs). */
	if (status == BTRFS_OK && !stopped && replay != 0) {
		btrfs_key_t first = btrfs_key_make(ino, BTRFS_INODE_REF_KEY, 0ULL);

		for (uint32_t i = btrfs_replay_lower_bound(replay, &first); i < replay->count && !stopped; i++) {
			const btrfs_log_item_t *item = &replay->items[i];
			const uint8_t *data = btrfs_replay_bytes(replay, item);
			uint32_t position = 0U;

			if (item->key.objectid != ino || item->key.type != BTRFS_INODE_REF_KEY) break;

			while (position < item->size) {
				btrfs_inode_ref_t ref;

				if (item->size - position < 10U) {
					status = BTRFS_ERR_CORRUPT;
					break;
				}

				ref.parent = item->key.offset;
				ref.index = btrfs_get_le64(data + position);
				ref.name_len = btrfs_get_le16(data + position + 8U);
				ref.extended = false;

				if (ref.name_len == 0U || ref.name_len > BTRFS_NAME_LEN || item->size - position - 10U < ref.name_len) {
					status = BTRFS_ERR_CORRUPT;
					break;
				}

				memcpy(ref.name, data + position + 10U, ref.name_len);
				ref.name[ref.name_len] = '\0';
				position += 10U + ref.name_len;
				visited++;

				if (callback != 0 && !callback(context, &ref)) {
					stopped = true;
					break;
				}
			}

			if (status != BTRFS_OK) break;
		}
	}

	if (count != 0) *count = visited;
	return status;
}
