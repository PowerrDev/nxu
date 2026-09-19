/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_inode.c
 *
 * INODE_ITEM reading and INODE_REF / INODE_EXTREF enumeration.
 */

#include "btrfs_inode.h"
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

btrfs_status_t btrfs_inode_read(btrfs_fs_t *fs, const btrfs_tree_t *subvol, uint64_t ino, btrfs_inode_t *out)
{
	btrfs_path_t path;
	btrfs_key_t key = btrfs_key_make(ino, BTRFS_INODE_ITEM_KEY, 0ULL);
	const uint8_t *data;
	uint32_t size;

	btrfs_path_init(&path);
	btrfs_status_t status = btrfs_tree_lookup(fs, subvol, &key, &path);
	if (status != BTRFS_OK) return status;

	btrfs_inode_item_t item;
	bool ok = btrfs_path_item(&path, 0, &data, &size) && btrfs_inode_item_parse(data, size, &item);
	btrfs_path_release(fs, &path);

	if (!ok) return BTRFS_ERR_CORRUPT;
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

		if (status != BTRFS_OK || stop) break;
		status = btrfs_path_next(fs, &path);
	}

	btrfs_path_release(fs, &path);
	if (count != 0) *count = visited;
	return status == BTRFS_ERR_END ? BTRFS_OK : status;
}
