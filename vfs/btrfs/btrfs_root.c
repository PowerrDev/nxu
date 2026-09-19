/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_root.c
 *
 * Root tree lookups. See btrfs_root.h.
 */

#include "btrfs_root.h"
#include "btrfs_tree.h"

#include <string.h>

btrfs_status_t btrfs_root_lookup(btrfs_fs_t *fs, uint64_t id, btrfs_subvol_t *out)
{
	btrfs_path_t path;
	btrfs_key_t target = btrfs_key_make(id, BTRFS_ROOT_ITEM_KEY, UINT64_MAX);
	btrfs_key_t key;
	const uint8_t *data;
	uint32_t size;

	btrfs_path_init(&path);
	btrfs_status_t status = btrfs_tree_search_le(fs, &fs->root_tree, &target, &path, 0);

	if (status == BTRFS_ERR_END) return BTRFS_ERR_NOT_FOUND;
	if (status != BTRFS_OK) return status;

	if (!btrfs_path_item(&path, &key, &data, &size) || key.objectid != id || key.type != BTRFS_ROOT_ITEM_KEY) {
		btrfs_path_release(fs, &path);
		return BTRFS_ERR_NOT_FOUND;
	}

	btrfs_root_item_t item;
	bool parsed = btrfs_root_item_parse(data, size, &item);
	btrfs_path_release(fs, &path);

	if (!parsed) return BTRFS_ERR_CORRUPT;

	if (item.bytenr == 0ULL || (item.bytenr & (fs->sectorsize - 1U)) != 0ULL || item.level >= BTRFS_MAX_LEVEL) {
		BTRFS_LOG(fs, "root item of tree %llu is malformed (bytenr %llu level %u)", BTRFS_U64(id), BTRFS_U64(item.bytenr), (unsigned)item.level);
		return BTRFS_ERR_CORRUPT;
	}

	out->id = id;
	out->item = item;
	out->tree.objectid = id;
	out->tree.bytenr = item.bytenr;
	out->tree.generation = item.generation;
	out->tree.level = item.level;
	out->read_only = (item.flags & BTRFS_ROOT_SUBVOL_RDONLY) != 0ULL;
	return BTRFS_OK;
}

btrfs_status_t btrfs_root_default_id(btrfs_fs_t *fs, uint64_t *id)
{
	static const uint8_t name[] = { 'd', 'e', 'f', 'a', 'u', 'l', 't' };
	btrfs_key_t target = btrfs_key_make(BTRFS_ROOT_TREE_DIR_OBJECTID, BTRFS_DIR_ITEM_KEY, btrfs_name_hash(name, sizeof(name)));
	btrfs_path_t path;
	const uint8_t *data;
	uint32_t size;

	*id = BTRFS_FS_TREE_OBJECTID;

	btrfs_path_init(&path);
	btrfs_status_t status = btrfs_tree_lookup(fs, &fs->root_tree, &target, &path);

	if (status == BTRFS_ERR_NOT_FOUND) return BTRFS_OK; /* no "default" entry: the top-level subvolume */
	if (status != BTRFS_OK) return status;

	status = BTRFS_OK;
	if (!btrfs_path_item(&path, 0, &data, &size)) {
		status = BTRFS_ERR_CORRUPT;
	} else {
		size_t position = 0U;
		bool found = false;

		while (position < size) {
			btrfs_dir_item_t entry;
			size_t used;

			if (!btrfs_dir_item_parse(data + position, size - position, &entry, &used)) {
				status = BTRFS_ERR_CORRUPT;
				break;
			}

			if (entry.name_len == sizeof(name) && memcmp(entry.name, name, sizeof(name)) == 0) {
				if (entry.location.type != BTRFS_ROOT_ITEM_KEY) {
					status = BTRFS_ERR_CORRUPT;
				} else {
					*id = entry.location.objectid;
					found = true;
				}
				break;
			}

			position += used;
		}

		(void)found;
	}

	btrfs_path_release(fs, &path);
	return status;
}

btrfs_status_t btrfs_root_select_default(btrfs_fs_t *fs)
{
	uint64_t id;
	btrfs_status_t status = btrfs_root_default_id(fs, &id);

	if (status != BTRFS_OK) return status;
	fs->default_subvol = id;

	uint64_t wanted = fs->options.subvol_id != 0ULL ? fs->options.subvol_id : id;

	/* Only FS_TREE and subvolumes (ids >= 256) are file trees. */
	if (wanted != BTRFS_FS_TREE_OBJECTID && (wanted < BTRFS_FIRST_FREE_OBJECTID || wanted > BTRFS_LAST_FREE_OBJECTID)) {
		BTRFS_LOG(fs, "tree %llu is not a subvolume", BTRFS_U64(wanted));
		return BTRFS_ERR_NOT_FOUND;
	}

	btrfs_subvol_t subvol;
	status = btrfs_root_lookup(fs, wanted, &subvol);
	if (status != BTRFS_OK) {
		BTRFS_LOG(fs, "subvolume %llu: %s", BTRFS_U64(wanted), btrfs_status_name(status));
		return status;
	}

	fs->mount_subvol = wanted;
	return BTRFS_OK;
}

static btrfs_status_t btrfs_root_ref_read(const uint8_t *data, uint32_t size, uint64_t parent, uint64_t child, btrfs_root_ref_info_t *out)
{
	btrfs_root_ref_t ref;

	if (!btrfs_root_ref_parse(data, size, &ref)) return BTRFS_ERR_CORRUPT;

	out->parent_id = parent;
	out->child_id = child;
	out->dirid = ref.dirid;
	out->sequence = ref.sequence;
	out->name_len = ref.name_len;
	memcpy(out->name, ref.name, ref.name_len);
	out->name[ref.name_len] = '\0';
	return BTRFS_OK;
}

btrfs_status_t btrfs_root_backref(btrfs_fs_t *fs, uint64_t child_id, btrfs_root_ref_info_t *out)
{
	btrfs_path_t path;
	btrfs_key_t target = btrfs_key_make(child_id, BTRFS_ROOT_BACKREF_KEY, 0ULL);
	btrfs_key_t key;
	const uint8_t *data;
	uint32_t size;

	btrfs_path_init(&path);
	btrfs_status_t status = btrfs_tree_search_ge(fs, &fs->root_tree, &target, &path, 0);

	if (status == BTRFS_ERR_END) return BTRFS_ERR_NOT_FOUND;
	if (status != BTRFS_OK) return status;

	if (!btrfs_path_item(&path, &key, &data, &size) || key.objectid != child_id || key.type != BTRFS_ROOT_BACKREF_KEY) {
		btrfs_path_release(fs, &path);
		return BTRFS_ERR_NOT_FOUND;
	}

	status = btrfs_root_ref_read(data, size, key.offset, child_id, out);
	btrfs_path_release(fs, &path);
	return status;
}

btrfs_status_t btrfs_root_next_child(btrfs_fs_t *fs, uint64_t parent_id, uint64_t *cursor, btrfs_root_ref_info_t *out)
{
	btrfs_path_t path;
	btrfs_key_t target = btrfs_key_make(parent_id, BTRFS_ROOT_REF_KEY, *cursor);
	btrfs_key_t key;
	const uint8_t *data;
	uint32_t size;

	btrfs_path_init(&path);
	btrfs_status_t status = btrfs_tree_search_ge(fs, &fs->root_tree, &target, &path, 0);

	if (status != BTRFS_OK) return status;

	if (!btrfs_path_item(&path, &key, &data, &size) || key.objectid != parent_id || key.type != BTRFS_ROOT_REF_KEY) {
		btrfs_path_release(fs, &path);
		return BTRFS_ERR_END;
	}

	status = btrfs_root_ref_read(data, size, parent_id, key.offset, out);
	if (status == BTRFS_OK) *cursor = key.offset + 1ULL;
	btrfs_path_release(fs, &path);
	return status;
}

btrfs_status_t btrfs_root_next_subvol(btrfs_fs_t *fs, uint64_t *cursor, uint64_t *id)
{
	btrfs_path_t path;
	uint64_t start = *cursor < BTRFS_FS_TREE_OBJECTID ? BTRFS_FS_TREE_OBJECTID : *cursor;

	/* 6..255 are not file trees; jump from FS_TREE straight to the first subvolume id. */
	if (start > BTRFS_FS_TREE_OBJECTID && start < BTRFS_FIRST_FREE_OBJECTID) start = BTRFS_FIRST_FREE_OBJECTID;

	btrfs_key_t target = btrfs_key_make(start, BTRFS_ROOT_ITEM_KEY, 0ULL);
	btrfs_key_t key;

	btrfs_path_init(&path);
	btrfs_status_t status = btrfs_tree_search_ge(fs, &fs->root_tree, &target, &path, 0);

	while (status == BTRFS_OK) {
		if (!btrfs_path_item(&path, &key, 0, 0)) {
			status = BTRFS_ERR_CORRUPT;
			break;
		}

		if (key.objectid > BTRFS_LAST_FREE_OBJECTID) {
			status = BTRFS_ERR_END;
			break;
		}

		if (key.type == BTRFS_ROOT_ITEM_KEY && (key.objectid == BTRFS_FS_TREE_OBJECTID || key.objectid >= BTRFS_FIRST_FREE_OBJECTID)) {
			*id = key.objectid;
			*cursor = key.objectid + 1ULL;
			btrfs_path_release(fs, &path);
			return BTRFS_OK;
		}

		status = btrfs_path_next(fs, &path);
	}

	btrfs_path_release(fs, &path);
	return status;
}

btrfs_status_t btrfs_root_resolve_point(btrfs_fs_t *fs, uint64_t parent_id, uint64_t dir_ino, const uint8_t *name, size_t name_len, uint64_t child_id, btrfs_subvol_t *out)
{
	btrfs_path_t path;
	btrfs_key_t target = btrfs_key_make(parent_id, BTRFS_ROOT_REF_KEY, child_id);
	const uint8_t *data;
	uint32_t size;

	btrfs_path_init(&path);
	btrfs_status_t status = btrfs_tree_lookup(fs, &fs->root_tree, &target, &path);
	if (status != BTRFS_OK) return status;

	btrfs_root_ref_t ref;
	bool matches = false;

	if (!btrfs_path_item(&path, 0, &data, &size) || !btrfs_root_ref_parse(data, size, &ref)) {
		status = BTRFS_ERR_CORRUPT;
	} else {
		matches = ref.dirid == dir_ino && ref.name_len == name_len && memcmp(ref.name, name, name_len) == 0;
	}

	btrfs_path_release(fs, &path);
	if (status != BTRFS_OK) return status;
	if (!matches) return BTRFS_ERR_NOT_FOUND;

	status = btrfs_root_lookup(fs, child_id, out);
	if (status != BTRFS_OK) return status;

	/* refs == 0: the subvolume is deleted and only waiting to be cleaned up. */
	if (out->item.refs == 0U) return BTRFS_ERR_NOT_FOUND;
	return BTRFS_OK;
}
