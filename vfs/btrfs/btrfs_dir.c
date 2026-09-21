/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_dir.c
 *
 * Directory lookup and iteration. See btrfs_dir.h.
 */

#include "btrfs_dir.h"
#include "btrfs_replay.h"
#include "btrfs_tree.h"

#include <string.h>

/* Validate one parsed entry and copy it into the caller's form. */
static btrfs_status_t btrfs_dirent_fill(const btrfs_dir_item_t *entry, uint64_t index, btrfs_dirent_t *out)
{
	if (entry->type == BTRFS_FT_UNKNOWN || entry->type >= BTRFS_FT_XATTR) return BTRFS_ERR_CORRUPT;
	if (entry->location.type != BTRFS_INODE_ITEM_KEY && entry->location.type != BTRFS_ROOT_ITEM_KEY) return BTRFS_ERR_CORRUPT;

	/* A file name can hold anything but NUL and '/'. */
	for (uint32_t position = 0U; position < entry->name_len; position++) {
		if (entry->name[position] == 0U || entry->name[position] == '/') return BTRFS_ERR_CORRUPT;
	}

	out->location = entry->location;
	out->transid = entry->transid;
	out->index = index;
	out->type = entry->type;
	out->name_len = entry->name_len;
	memcpy(out->name, entry->name, entry->name_len);
	out->name[entry->name_len] = '\0';
	return BTRFS_OK;
}

static btrfs_status_t btrfs_dir_lookup_committed(btrfs_fs_t *fs, const btrfs_tree_t *subvol, uint64_t dir, const uint8_t *name, size_t length, btrfs_dirent_t *out)
{
	if (length == 0U) return BTRFS_ERR_NOT_FOUND;
	if (length > BTRFS_NAME_LEN) return BTRFS_ERR_NAME_TOO_LONG;

	btrfs_key_t key = btrfs_key_make(dir, BTRFS_DIR_ITEM_KEY, btrfs_name_hash(name, length));
	btrfs_path_t path;
	const uint8_t *data;
	uint32_t size;

	btrfs_path_init(&path);
	btrfs_status_t status = btrfs_tree_lookup(fs, subvol, &key, &path);
	if (status != BTRFS_OK) return status;

	if (!btrfs_path_item(&path, 0, &data, &size)) {
		btrfs_path_release(fs, &path);
		return BTRFS_ERR_CORRUPT;
	}

	status = BTRFS_ERR_NOT_FOUND;

	/* Names with the same hash share the item; walk them all and check the item tiles exactly. */
	size_t position = 0U;
	while (position < size) {
		btrfs_dir_item_t entry;
		size_t used;

		if (!btrfs_dir_item_parse(data + position, size - position, &entry, &used)) {
			status = BTRFS_ERR_CORRUPT;
			break;
		}

		if (status == BTRFS_ERR_NOT_FOUND && entry.name_len == length && memcmp(entry.name, name, length) == 0) {
			status = btrfs_dirent_fill(&entry, 0ULL, out);
			if (status != BTRFS_OK) break;
			status = BTRFS_OK;
		}

		position += used;
	}

	btrfs_path_release(fs, &path);
	return status;
}

btrfs_status_t btrfs_dir_next_committed(btrfs_fs_t *fs, const btrfs_tree_t *subvol, uint64_t dir, uint64_t *cursor, btrfs_dirent_t *out)
{
	btrfs_key_t target = btrfs_key_make(dir, BTRFS_DIR_INDEX_KEY, *cursor);
	btrfs_path_t path;
	btrfs_key_t key;
	const uint8_t *data;
	uint32_t size;

	btrfs_path_init(&path);
	btrfs_status_t status = btrfs_tree_search_ge(fs, subvol, &target, &path, 0);
	if (status != BTRFS_OK) return status;

	if (!btrfs_path_item(&path, &key, &data, &size)) {
		btrfs_path_release(fs, &path);
		return BTRFS_ERR_CORRUPT;
	}

	if (key.objectid != dir || key.type != BTRFS_DIR_INDEX_KEY) {
		btrfs_path_release(fs, &path);
		return BTRFS_ERR_END;
	}

	btrfs_dir_item_t entry;
	size_t used;

	if (!btrfs_dir_item_parse(data, size, &entry, &used) || used != size) {
		status = BTRFS_ERR_CORRUPT;
	} else {
		status = btrfs_dirent_fill(&entry, key.offset, out);
	}

	btrfs_path_release(fs, &path);

	if (status == BTRFS_OK) {
		if (key.offset == UINT64_MAX) return BTRFS_ERR_CORRUPT;
		*cursor = key.offset + 1ULL;
	}

	return status;
}

/* ---- the log's view of a directory ---------------------------------------------------------- */

static void btrfs_dirent_from_log(const btrfs_replay_t *replay, const btrfs_log_dirent_t *logged, btrfs_dirent_t *out)
{
	out->location = btrfs_key_make(logged->ino, BTRFS_INODE_ITEM_KEY, 0ULL);
	out->transid = 0ULL;
	out->index = logged->index;
	out->type = logged->type;
	out->name_len = logged->name_len;
	memcpy(out->name, replay->data + logged->name_offset, logged->name_len);
	out->name[logged->name_len] = '\0';
}

/*
 * By index, the entry the log has, else the committed one unless the log removes
 * it: what a replay would leave. Log entries win an index they share with the
 * committed tree (Linux unlinks the old name there).
 */
btrfs_status_t btrfs_dir_next(btrfs_fs_t *fs, const btrfs_tree_t *subvol, uint64_t dir, uint64_t *cursor, btrfs_dirent_t *out)
{
	const btrfs_replay_t *replay = btrfs_replay_find(fs, subvol->objectid);

	if (replay == 0 || !btrfs_replay_dir_touched(replay, dir)) return btrfs_dir_next_committed(fs, subvol, dir, cursor, out);

	uint64_t from = *cursor;

	for (;;) {
		const btrfs_log_dirent_t *logged = btrfs_replay_dirent_ge(replay, dir, from);
		uint64_t committed_cursor = from;
		btrfs_dirent_t committed;
		btrfs_status_t status = btrfs_dir_next_committed(fs, subvol, dir, &committed_cursor, &committed);

		if (status != BTRFS_OK && status != BTRFS_ERR_END) return status;

		if (logged != 0 && (status != BTRFS_OK || logged->index <= committed.index)) {
			if (logged->index == UINT64_MAX) return BTRFS_ERR_CORRUPT;

			btrfs_dirent_from_log(replay, logged, out);
			*cursor = logged->index + 1ULL;
			return BTRFS_OK;
		}

		if (status == BTRFS_ERR_END) return BTRFS_ERR_END;

		if (!btrfs_replay_dir_removes(replay, dir, committed.index)) {
			*out = committed;
			*cursor = committed_cursor;
			return BTRFS_OK;
		}

		from = committed_cursor;
	}
}

btrfs_status_t btrfs_dir_lookup(btrfs_fs_t *fs, const btrfs_tree_t *subvol, uint64_t dir, const uint8_t *name, size_t length, btrfs_dirent_t *out)
{
	const btrfs_replay_t *replay = btrfs_replay_find(fs, subvol->objectid);

	if (replay == 0 || !btrfs_replay_dir_touched(replay, dir)) return btrfs_dir_lookup_committed(fs, subvol, dir, name, length, out);

	if (length == 0U) return BTRFS_ERR_NOT_FOUND;
	if (length > BTRFS_NAME_LEN) return BTRFS_ERR_NAME_TOO_LONG;

	/* A directory the log touched: walk the merged listing (names are hashed only in the committed tree). */
	uint64_t cursor = 0ULL;

	for (;;) {
		btrfs_status_t status = btrfs_dir_next(fs, subvol, dir, &cursor, out);

		if (status == BTRFS_ERR_END) return BTRFS_ERR_NOT_FOUND;
		if (status != BTRFS_OK) return status;

		if (out->name_len == length && memcmp(out->name, name, length) == 0) {
			out->index = 0ULL;
			return BTRFS_OK;
		}
	}
}
