/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_file.c
 *
 * The data read path. See btrfs_file.h for the extent model.
 *
 * btrfs_file_read positions a path on the last EXTENT_DATA item at or before
 * the requested offset and then walks forward item by item, copying from each
 * extent (or zero-filling holes) until the range is done. Every iteration
 * either advances the file position or moves to a strictly later item, so a
 * corrupt extent list cannot make it loop.
 */

#include "btrfs_file.h"
#include "btrfs_root.h"
#include "btrfs_tree.h"

#include <string.h>

#define BTRFS_MAX_EXTENT_BYTES (1U << 20U)   /* sanity bound for decoded/compressed extents */
#define BTRFS_VERIFY_CHUNK 65536U

/* ---- data checksums ---------------------------------------------------------------------- */

static unsigned btrfs_log2_u32(uint32_t value)
{
	unsigned shift = 0U;
	while ((1U << shift) < value) shift++;
	return shift;
}

static btrfs_status_t btrfs_csum_tree_open(btrfs_fs_t *fs)
{
	if (fs->have_csum_tree) return BTRFS_OK;

	btrfs_subvol_t root;
	btrfs_status_t status = btrfs_root_lookup(fs, BTRFS_CSUM_TREE_OBJECTID, &root);
	if (status != BTRFS_OK) return status;

	fs->csum_tree = root.tree;
	fs->have_csum_tree = true;
	return BTRFS_OK;
}

/*
 * Check `length` bytes (whole sectors) read from sector-aligned logical
 * against the csum tree. A sector with no csum item is not checked (data of a
 * NODATASUM file, or a filesystem written without data checksums).
 */
static btrfs_status_t btrfs_verify_sectors(btrfs_fs_t *fs, uint64_t logical, const uint8_t *data, size_t length)
{
	uint32_t sector = fs->sectorsize;
	unsigned shift = btrfs_log2_u32(sector);
	btrfs_path_t path;
	const uint8_t *window = 0;
	uint64_t window_start = 0ULL;
	uint64_t window_end = 0ULL;

	btrfs_path_init(&path);

	for (size_t position = 0U; position < length; position += sector) {
		uint64_t address = logical + position;

		if (window == 0 || address < window_start || address >= window_end) {
			btrfs_key_t target = btrfs_key_make(BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, address);
			btrfs_key_t key;
			const uint8_t *item;
			uint32_t size;

			window = 0;
			btrfs_status_t status = btrfs_tree_search_le(fs, &fs->csum_tree, &target, &path, 0);
			if (status == BTRFS_ERR_END) continue;
			if (status != BTRFS_OK) return status;

			if (btrfs_path_item(&path, &key, &item, &size) && key.objectid == BTRFS_EXTENT_CSUM_OBJECTID && key.type == BTRFS_EXTENT_CSUM_KEY) {
				uint64_t covered = ((uint64_t)(size / 4U)) << shift;
				if (address >= key.offset && address - key.offset < covered) {
					window = item;
					window_start = key.offset;
					window_end = key.offset + covered;
				}
			}

			if (window == 0) continue;
		}

		uint32_t expected = btrfs_get_le32(window + (((address - window_start) >> shift) * 4U));
		fs->stats.data_csum_checked++;

		if (expected != btrfs_csum_crc32c(data + position, sector)) {
			btrfs_path_release(fs, &path);
			fs->stats.csum_failures++;
			BTRFS_LOG(fs, "data checksum mismatch at logical %llu", BTRFS_U64(address));
			return BTRFS_ERR_CSUM;
		}
	}

	btrfs_path_release(fs, &path);
	return BTRFS_OK;
}

/* ---- device reads of data ------------------------------------------------------------------- */

/* Read logical range [logical, logical+length) trying each copy; no checksum. */
static btrfs_status_t btrfs_read_data_plain(btrfs_fs_t *fs, uint64_t logical, uint8_t *buffer, size_t length)
{
	while (length != 0U) {
		btrfs_mapping_t mapping;
		btrfs_status_t status = btrfs_map_logical(fs, logical, &mapping);
		if (status != BTRFS_OK) return status;

		size_t amount = (uint64_t)length < mapping.length ? length : (size_t)mapping.length;
		status = BTRFS_ERR_IO;

		for (uint32_t copy = 0U; copy < mapping.copies && status != BTRFS_OK; copy++) {
			status = btrfs_read_logical(fs, logical, buffer, amount, copy);
			if (status != BTRFS_OK && copy + 1U < mapping.copies) fs->stats.mirror_fallbacks++;
		}

		if (status != BTRFS_OK) return status;

		logical += amount;
		buffer += amount;
		length -= amount;
	}

	return BTRFS_OK;
}

/*
 * Read with checksum verification: the range is widened to whole sectors, read
 * in chunks, and each chunk must verify on some copy of its chunk mapping.
 */
static btrfs_status_t btrfs_read_data_verified(btrfs_fs_t *fs, uint64_t logical, uint8_t *buffer, size_t length)
{
	uint64_t mask = fs->sectorsize - 1U;
	uint64_t start = logical & ~mask;
	uint64_t end = (logical + length + mask) & ~mask;

	btrfs_status_t status = btrfs_csum_tree_open(fs);
	if (status != BTRFS_OK) {
		/* No csum tree at all: nothing to verify against. */
		return btrfs_read_data_plain(fs, logical, buffer, length);
	}

	uint8_t *scratch = btrfs_alloc(fs, BTRFS_VERIFY_CHUNK);
	if (scratch == 0) return BTRFS_ERR_NOMEM;

	for (uint64_t position = start; position < end; position += BTRFS_VERIFY_CHUNK) {
		size_t amount = end - position < BTRFS_VERIFY_CHUNK ? (size_t)(end - position) : BTRFS_VERIFY_CHUNK;
		btrfs_mapping_t mapping;

		status = btrfs_map_logical(fs, position, &mapping);
		if (status != BTRFS_OK) break;
		if ((uint64_t)amount > mapping.length) {
			status = BTRFS_ERR_CORRUPT;
			break;
		}

		status = BTRFS_ERR_IO;
		for (uint32_t copy = 0U; copy < mapping.copies && status != BTRFS_OK; copy++) {
			status = btrfs_read_logical(fs, position, scratch, amount, copy);
			if (status == BTRFS_OK) status = btrfs_verify_sectors(fs, position, scratch, amount);
			if (status != BTRFS_OK && copy + 1U < mapping.copies) fs->stats.mirror_fallbacks++;
		}

		if (status != BTRFS_OK) break;

		/* Copy out the part of this chunk the caller asked for. */
		uint64_t copy_start = position < logical ? logical : position;
		uint64_t copy_end = position + amount > logical + length ? logical + length : position + amount;
		if (copy_end > copy_start) memcpy(buffer + (copy_start - logical), scratch + (copy_start - position), (size_t)(copy_end - copy_start));
	}

	btrfs_free(fs, scratch);
	return status;
}

/* ---- one extent ------------------------------------------------------------------------------ */

typedef struct {
	btrfs_key_t key;
	btrfs_file_extent_t extent;
	const uint8_t *item;     /* the item bytes (pinned by the path) */
	uint32_t item_size;
	uint64_t length;         /* bytes of the file this extent covers */
} btrfs_extent_view_t;

static btrfs_status_t btrfs_extent_parse(btrfs_fs_t *fs, const btrfs_key_t *key, const uint8_t *item, uint32_t size, btrfs_extent_view_t *view)
{
	if (!btrfs_file_extent_parse(item, size, &view->extent)) return BTRFS_ERR_CORRUPT;

	const btrfs_file_extent_t *e = &view->extent;
	uint32_t sector_mask = fs->sectorsize - 1U;

	view->key = *key;
	view->item = item;
	view->item_size = size;

	if (e->type == BTRFS_FILE_EXTENT_INLINE) {
		/* An inline extent is the file's first bytes; the decoded length is ram_bytes. */
		if (key->offset != 0ULL) return BTRFS_ERR_CORRUPT;
		if (e->ram_bytes == 0ULL || e->ram_bytes > BTRFS_MAX_EXTENT_BYTES) return BTRFS_ERR_CORRUPT;
		if (e->compression == BTRFS_COMPRESS_NONE && e->ram_bytes != e->inline_size) return BTRFS_ERR_CORRUPT;
		view->length = e->ram_bytes;
		return BTRFS_OK;
	}

	if (e->num_bytes == 0ULL || key->offset + e->num_bytes < key->offset) return BTRFS_ERR_CORRUPT;
	view->length = e->num_bytes;

	if (e->disk_bytenr != 0ULL) {
		if ((e->disk_bytenr & sector_mask) != 0ULL || e->disk_num_bytes == 0ULL) return BTRFS_ERR_CORRUPT;
		if (e->disk_bytenr + e->disk_num_bytes < e->disk_bytenr) return BTRFS_ERR_CORRUPT;

		if (e->compression == BTRFS_COMPRESS_NONE) {
			/* The slice [offset, offset+num_bytes) must lie inside the disk extent. */
			if (e->offset > e->disk_num_bytes || e->num_bytes > e->disk_num_bytes - e->offset) return BTRFS_ERR_CORRUPT;
		} else {
			/* Inside the decoded extent instead. */
			if (e->ram_bytes == 0ULL || e->ram_bytes > BTRFS_MAX_EXTENT_BYTES) return BTRFS_ERR_CORRUPT;
			if (e->disk_num_bytes > BTRFS_MAX_EXTENT_BYTES) return BTRFS_ERR_CORRUPT;
			if (e->offset > e->ram_bytes || e->num_bytes > e->ram_bytes - e->offset) return BTRFS_ERR_CORRUPT;
		}
	}

	return BTRFS_OK;
}

/* Decode a compressed extent (or inline data) and copy the slice out of it. */
static btrfs_status_t btrfs_read_compressed(btrfs_fs_t *fs, const btrfs_extent_view_t *view, uint64_t skip, uint8_t *buffer, size_t amount)
{
	const btrfs_file_extent_t *e = &view->extent;

	if (e->compression >= 4U || fs->decompressors[e->compression].decompress == 0) return BTRFS_ERR_UNSUPPORTED_COMPRESSION;

	size_t decoded = (size_t)e->ram_bytes;
	const uint8_t *packed;
	uint8_t *packed_copy = 0;
	size_t packed_size;

	uint8_t *out = btrfs_alloc(fs, decoded);
	if (out == 0) return BTRFS_ERR_NOMEM;

	if (e->type == BTRFS_FILE_EXTENT_INLINE) {
		packed = view->item + BTRFS_FILE_EXTENT_INLINE_HEADER;
		packed_size = e->inline_size;
	} else {
		packed_size = (size_t)e->disk_num_bytes;
		packed_copy = btrfs_alloc(fs, packed_size);
		if (packed_copy == 0) {
			btrfs_free(fs, out);
			return BTRFS_ERR_NOMEM;
		}

		btrfs_status_t status = btrfs_read_data_plain(fs, e->disk_bytenr, packed_copy, packed_size);
		if (status != BTRFS_OK) {
			btrfs_free(fs, packed_copy);
			btrfs_free(fs, out);
			return status;
		}
		packed = packed_copy;
	}

	btrfs_status_t status = fs->decompressors[e->compression].decompress(fs->decompressors[e->compression].ctx, packed, packed_size, out, decoded);
	if (status == BTRFS_OK) {
		uint64_t start = (e->type == BTRFS_FILE_EXTENT_INLINE ? 0ULL : e->offset) + skip;
		if (start > decoded || amount > decoded - (size_t)start) status = BTRFS_ERR_CORRUPT;
		else memcpy(buffer, out + (size_t)start, amount);
	}

	btrfs_free(fs, packed_copy);
	btrfs_free(fs, out);
	return status;
}

/*
 * Copy `amount` bytes of the file starting `skip` bytes into this extent.
 */
static btrfs_status_t btrfs_read_extent(btrfs_fs_t *fs, const btrfs_inode_t *inode, const btrfs_extent_view_t *view, uint64_t skip, uint8_t *buffer, size_t amount)
{
	const btrfs_file_extent_t *e = &view->extent;

	if (e->encryption != 0U || e->other_encoding != 0U) return BTRFS_ERR_UNSUPPORTED_ENCRYPTION;

	if (e->type == BTRFS_FILE_EXTENT_INLINE) {
		if (e->compression != BTRFS_COMPRESS_NONE) return btrfs_read_compressed(fs, view, skip, buffer, amount);
		if (skip > e->inline_size || amount > e->inline_size - skip) return BTRFS_ERR_CORRUPT;
		memcpy(buffer, view->item + BTRFS_FILE_EXTENT_INLINE_HEADER + skip, amount);
		return BTRFS_OK;
	}

	/* Preallocated space and explicit holes read as zeros. */
	if (e->type == BTRFS_FILE_EXTENT_PREALLOC || e->disk_bytenr == 0ULL) {
		memset(buffer, 0, amount);
		return BTRFS_OK;
	}

	if (e->compression != BTRFS_COMPRESS_NONE) return btrfs_read_compressed(fs, view, skip, buffer, amount);

	uint64_t logical = e->disk_bytenr + e->offset + skip;

	if (fs->options.verify_data_csums && (inode->item.flags & BTRFS_INODE_NODATASUM) == 0ULL) {
		return btrfs_read_data_verified(fs, logical, buffer, amount);
	}

	return btrfs_read_data_plain(fs, logical, buffer, amount);
}

/* ---- the read loop ---------------------------------------------------------------------------- */

btrfs_status_t btrfs_file_read(btrfs_fs_t *fs, const btrfs_tree_t *subvol, const btrfs_inode_t *inode, uint64_t offset, void *buffer, uint64_t length, uint64_t *done)
{
	uint8_t *out = buffer;
	uint64_t size = inode->item.size;
	uint32_t type = btrfs_inode_type(inode->item.mode);

	*done = 0ULL;

	if (type == BTRFS_S_IFDIR) return BTRFS_ERR_IS_DIRECTORY;
	if (type != BTRFS_S_IFREG && type != BTRFS_S_IFLNK) return BTRFS_ERR_INVALID;
	if (offset >= size || length == 0ULL) return BTRFS_OK;

	uint64_t end = offset + length < offset ? UINT64_MAX : offset + length;
	if (end > size) end = size;

	uint64_t position = offset;
	btrfs_path_t path;
	btrfs_key_t target = btrfs_key_make(inode->ino, BTRFS_EXTENT_DATA_KEY, position);
	btrfs_key_t key;
	const uint8_t *item;
	uint32_t item_size;

	btrfs_path_init(&path);

	/* Start on the last extent that begins at or before the offset. */
	btrfs_status_t status = btrfs_tree_search_le(fs, subvol, &target, &path, 0);
	bool positioned = status == BTRFS_OK;

	if (status != BTRFS_OK && status != BTRFS_ERR_END) return status;

	if (positioned && (!btrfs_path_item(&path, &key, 0, 0) || key.objectid != inode->ino || key.type != BTRFS_EXTENT_DATA_KEY)) {
		/* No extent starts at or before the offset: begin at the first one after it. */
		status = btrfs_tree_search_ge(fs, subvol, &target, &path, 0);
		positioned = status == BTRFS_OK;
		if (status != BTRFS_OK && status != BTRFS_ERR_END) return status;
	} else if (!positioned) {
		status = btrfs_tree_search_ge(fs, subvol, &target, &path, 0);
		positioned = status == BTRFS_OK;
		if (status != BTRFS_OK && status != BTRFS_ERR_END) return status;
	}

	status = BTRFS_OK;

	while (position < end) {
		bool have = positioned && btrfs_path_item(&path, &key, &item, &item_size) && key.objectid == inode->ino && key.type == BTRFS_EXTENT_DATA_KEY;

		if (!have) {
			/* Nothing but a hole (or the end of the tree) remains. */
			memset(out + (position - offset), 0, (size_t)(end - position));
			position = end;
			break;
		}

		btrfs_extent_view_t view;
		status = btrfs_extent_parse(fs, &key, item, item_size, &view);
		if (status != BTRFS_OK) {
			BTRFS_LOG(fs, "inode %llu: bad extent item at file offset %llu", BTRFS_U64(inode->ino), BTRFS_U64(key.offset));
			break;
		}

		uint64_t extent_start = key.offset;
		uint64_t extent_end = extent_start + view.length;

		if (extent_start > position) {
			/* A gap before this extent: a hole. */
			uint64_t hole_end = extent_start < end ? extent_start : end;
			memset(out + (position - offset), 0, (size_t)(hole_end - position));
			position = hole_end;
			continue;
		}

		if (extent_end > position) {
			uint64_t stop = extent_end < end ? extent_end : end;
			size_t amount = (size_t)(stop - position);

			status = btrfs_read_extent(fs, inode, &view, position - extent_start, out + (position - offset), amount);
			if (status != BTRFS_OK) break;

			position = stop;
			if (position >= end) break;
		}

		status = btrfs_path_next(fs, &path);
		if (status == BTRFS_ERR_END) {
			positioned = false;
			status = BTRFS_OK;
		} else if (status != BTRFS_OK) {
			break;
		}
	}

	btrfs_path_release(fs, &path);

	if (status != BTRFS_OK) return status;

	*done = end - offset;
	return BTRFS_OK;
}

btrfs_status_t btrfs_file_readlink(btrfs_fs_t *fs, const btrfs_tree_t *subvol, const btrfs_inode_t *inode, char *buffer, size_t capacity, size_t *length)
{
	uint64_t done;

	*length = 0U;
	if (btrfs_inode_type(inode->item.mode) != BTRFS_S_IFLNK) return BTRFS_ERR_INVALID;
	if (inode->item.size > capacity) return BTRFS_ERR_NAME_TOO_LONG;

	btrfs_status_t status = btrfs_file_read(fs, subvol, inode, 0ULL, buffer, inode->item.size, &done);
	if (status != BTRFS_OK) return status;
	if (done != inode->item.size) return BTRFS_ERR_CORRUPT;

	*length = (size_t)done;
	return BTRFS_OK;
}
