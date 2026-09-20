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

#include "btrfs_codec.h"
#include "btrfs_file.h"
#include "btrfs_root.h"
#include "btrfs_tree.h"

#include <string.h>

#define BTRFS_VERIFY_CHUNK 65536U

/* ---- data checksums ---------------------------------------------------------------------- */

static unsigned btrfs_log2_u32(uint32_t value)
{
	unsigned shift = 0U;
	while ((1U << shift) < value) shift++;
	return shift;
}

/*
 * A cursor over the csum tree. EXTENT_CSUM items are (EXTENT_CSUM, EXTENT_CSUM,
 * logical) and hold one checksum per sector for as many sectors as fit, so a
 * run of extents is covered by a few items. The cursor keeps the current item
 * pinned and only searches again when an address falls outside it; the leaves
 * come from the tree-block cache. btrfs_csum_scan_done() must be called once.
 */
typedef struct {
	btrfs_path_t path;
	const uint8_t *window;   /* checksums of [window_start, window_end), pinned by path */
	uint64_t window_start;
	uint64_t window_end;
} btrfs_csum_scan_t;

static void btrfs_csum_scan_init(btrfs_csum_scan_t *scan)
{
	btrfs_path_init(&scan->path);
	scan->window = 0;
	scan->window_start = 0ULL;
	scan->window_end = 0ULL;
}

static void btrfs_csum_scan_done(btrfs_fs_t *fs, btrfs_csum_scan_t *scan)
{
	btrfs_path_release(fs, &scan->path);
	scan->window = 0;
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
 * Find the stored checksum of the sector at `address`. *expected is NULL when
 * the csum tree holds none (a hole in the coverage, or no csum tree at all);
 * the caller decides what that means. The pointer stays valid until the next
 * call on this cursor.
 */
static btrfs_status_t btrfs_csum_scan_find(btrfs_fs_t *fs, btrfs_csum_scan_t *scan, uint64_t address, const uint8_t **expected)
{
	*expected = 0;

	if (scan->window == 0 || address < scan->window_start || address >= scan->window_end) {
		scan->window = 0;

		btrfs_status_t status = btrfs_csum_tree_open(fs);
		if (status == BTRFS_ERR_NOT_FOUND) return BTRFS_OK;
		if (status != BTRFS_OK) return status;

		btrfs_key_t target = btrfs_key_make(BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, address);
		btrfs_key_t key;
		const uint8_t *item;
		uint32_t size;

		status = btrfs_tree_search_le(fs, &fs->csum_tree, &target, &scan->path, 0);
		if (status == BTRFS_ERR_END) return BTRFS_OK;
		if (status != BTRFS_OK) return status;

		if (!btrfs_path_item(&scan->path, &key, &item, &size) || key.objectid != BTRFS_EXTENT_CSUM_OBJECTID || key.type != BTRFS_EXTENT_CSUM_KEY) return BTRFS_OK;
		if ((key.offset & (fs->sectorsize - 1U)) != 0ULL) return BTRFS_ERR_CORRUPT;

		uint64_t covered = ((uint64_t)(size / fs->csum_size)) << btrfs_log2_u32(fs->sectorsize);
		if (key.offset + covered < key.offset) return BTRFS_ERR_CORRUPT;
		if (address < key.offset || address - key.offset >= covered) return BTRFS_OK;

		scan->window = item;
		scan->window_start = key.offset;
		scan->window_end = key.offset + covered;
	}

	*expected = scan->window + ((address - scan->window_start) >> btrfs_log2_u32(fs->sectorsize)) * fs->csum_size;
	return BTRFS_OK;
}

/*
 * Check `length` bytes (whole sectors) read from sector-aligned `logical`
 * against the csum tree. Every sector must have a checksum and it must match:
 * this is only called for the extents of a file that carries data checksums, so
 * a missing item means the volume lost it and the data cannot be vouched for.
 * Returns BTRFS_ERR_CSUM for a mismatch or a missing checksum; *missing tells
 * which (another copy of the data cannot supply a checksum that is not there).
 */
static btrfs_status_t btrfs_verify_sectors(btrfs_fs_t *fs, btrfs_csum_scan_t *scan, uint64_t logical, const uint8_t *data, size_t length, bool *missing)
{
	uint32_t sector = fs->sectorsize;

	*missing = false;

	for (size_t position = 0U; position < length; position += sector) {
		uint64_t address = logical + position;
		const uint8_t *expected;
		btrfs_status_t status = btrfs_csum_scan_find(fs, scan, address, &expected);
		if (status != BTRFS_OK) return status;

		if (expected == 0) {
			*missing = true;
			fs->stats.data_csum_missing++;
			BTRFS_LOG(fs, "no data checksum for logical %llu", BTRFS_U64(address));
			return BTRFS_ERR_CSUM;
		}

		fs->stats.data_csum_checked++;

		if (!btrfs_csum_matches(fs->csum_type, expected, data + position, sector)) {
			fs->stats.csum_failures++;
			BTRFS_LOG(fs, "data checksum mismatch at logical %llu", BTRFS_U64(address));
			return BTRFS_ERR_CSUM;
		}
	}

	return BTRFS_OK;
}

/* ---- device reads of data ------------------------------------------------------------------- */

/*
 * Read logical range [logical, logical+length) trying each copy, no checksum:
 * for files without data checksums (NODATASUM), and for mounts that opted out
 * with noverify.
 */
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
 * Read with checksum verification (the bad-extent policy of doc/vfs/btrfs.md):
 * the range is widened to whole sectors and read in chunks; each chunk must
 * verify on some copy of its chunk mapping (the first copy that verifies is
 * used and the failed ones are counted in mirror_fallbacks). Bytes reach
 * `buffer` only after the chunk they belong to verified. If no copy of a
 * chunk verifies, the whole read fails with BTRFS_ERR_CSUM (or the device's
 * error) and nothing further is read: a bad extent is an I/O error for the
 * read that touches it, never silently wrong data.
 */
static btrfs_status_t btrfs_read_data_verified(btrfs_fs_t *fs, uint64_t logical, uint8_t *buffer, size_t length)
{
	uint64_t mask = fs->sectorsize - 1U;
	uint64_t start = logical & ~mask;
	uint64_t end = (logical + length + mask) & ~mask;
	btrfs_status_t status = BTRFS_OK;

	uint8_t *scratch = btrfs_alloc(fs, BTRFS_VERIFY_CHUNK);
	if (scratch == 0) return BTRFS_ERR_NOMEM;

	btrfs_csum_scan_t scan;
	btrfs_csum_scan_init(&scan);

	for (uint64_t position = start; position < end;) {
		size_t amount = end - position < BTRFS_VERIFY_CHUNK ? (size_t)(end - position) : BTRFS_VERIFY_CHUNK;
		btrfs_mapping_t mapping;

		status = btrfs_map_logical(fs, position, &mapping);
		if (status != BTRFS_OK) break;

		/* An extent may straddle two chunks; a chunk boundary is always sector aligned. */
		if ((uint64_t)amount > mapping.length) amount = (size_t)mapping.length;

		bool missing = false;

		status = BTRFS_ERR_IO;
		for (uint32_t copy = 0U; copy < mapping.copies && status != BTRFS_OK; copy++) {
			status = btrfs_read_logical(fs, position, scratch, amount, copy);
			if (status == BTRFS_OK) status = btrfs_verify_sectors(fs, &scan, position, scratch, amount, &missing);

			/* Only a failed read or a mismatch is worth another copy. */
			if (status != BTRFS_OK && status != BTRFS_ERR_CSUM && status != BTRFS_ERR_IO) break;
			if (missing) break;
			if (status != BTRFS_OK && copy + 1U < mapping.copies) fs->stats.mirror_fallbacks++;
		}

		if (status != BTRFS_OK) break;

		/* Copy out the part of this chunk the caller asked for. */
		uint64_t copy_start = position < logical ? logical : position;
		uint64_t copy_end = position + amount > logical + length ? logical + length : position + amount;
		if (copy_end > copy_start) memcpy(buffer + (copy_start - logical), scratch + (copy_start - position), (size_t)(copy_end - copy_start));

		position += amount;
	}

	btrfs_csum_scan_done(fs, &scan);
	btrfs_free(fs, scratch);
	if (status == BTRFS_ERR_CSUM || status == BTRFS_ERR_IO) fs->stats.data_bad_reads++;
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
		if (e->ram_bytes == 0ULL || e->ram_bytes > BTRFS_MAX_COMPRESSED_EXTENT) return BTRFS_ERR_CORRUPT;
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
			if (e->ram_bytes == 0ULL || e->ram_bytes > BTRFS_MAX_COMPRESSED_EXTENT) return BTRFS_ERR_CORRUPT;
			if (e->disk_num_bytes > BTRFS_MAX_COMPRESSED_EXTENT) return BTRFS_ERR_CORRUPT;
			if (e->offset > e->ram_bytes || e->num_bytes > e->ram_bytes - e->offset) return BTRFS_ERR_CORRUPT;
		}
	}

	return BTRFS_OK;
}

/*
 * Read the extent's compressed bytes: the same policy as any data read (checksums
 * unless the file or the mount says otherwise; the csum covers what is on disk,
 * so the compressed form is what gets verified).
 */
static btrfs_status_t btrfs_read_data(btrfs_fs_t *fs, const btrfs_inode_t *inode, uint64_t logical, uint8_t *buffer, size_t length, bool *verified)
{
	*verified = !fs->options.skip_data_csums && (inode->item.flags & BTRFS_INODE_NODATASUM) == 0ULL;

	if (*verified) return btrfs_read_data_verified(fs, logical, buffer, length);
	return btrfs_read_data_plain(fs, logical, buffer, length);
}

/*
 * Decode a compressed extent (or inline data) and copy the slice out of it.
 *
 * Memory: the decoded extent is at most BTRFS_MAX_COMPRESSED_EXTENT bytes and
 * lives in fs->extent_cache, the one decoded extent that is kept, so reading a
 * file in small steps decodes each extent once. The cache buffer is allocated on
 * first use and released at close. While a decode runs there is also the
 * compressed copy (at most the same size) and the decoder's work memory (ZSTD:
 * tables plus one block of literals); both are freed before returning.
 * A failed decode leaves the cache empty. Inline extents are small: decoded
 * into a temporary buffer, not cached.
 */
static btrfs_status_t btrfs_read_compressed(btrfs_fs_t *fs, const btrfs_inode_t *inode, const btrfs_extent_view_t *view, uint64_t skip, uint8_t *buffer, size_t amount)
{
	const btrfs_file_extent_t *e = &view->extent;
	btrfs_extent_cache_t *cache = &fs->extent_cache;

	if (e->compression >= 4U || fs->decompressors[e->compression].decompress == 0) return BTRFS_ERR_UNSUPPORTED_COMPRESSION;
	if (e->ram_bytes == 0ULL || e->ram_bytes > BTRFS_MAX_COMPRESSED_EXTENT) return BTRFS_ERR_CORRUPT;

	size_t decoded = (size_t)e->ram_bytes;
	uint64_t start = (e->type == BTRFS_FILE_EXTENT_INLINE ? 0ULL : e->offset) + skip;
	bool inline_extent = e->type == BTRFS_FILE_EXTENT_INLINE;

	if (start > decoded || amount > decoded - (size_t)start) return BTRFS_ERR_CORRUPT;

	bool need_verified = !fs->options.skip_data_csums && (inode->item.flags & BTRFS_INODE_NODATASUM) == 0ULL;

	if (!inline_extent && cache->data != 0 && cache->disk_bytenr == e->disk_bytenr && cache->disk_num_bytes == e->disk_num_bytes && cache->ram_bytes == e->ram_bytes && cache->compression == e->compression && (cache->verified || !need_verified)) {
		fs->stats.extent_cache_hits++;
		memcpy(buffer, cache->data + (size_t)start, amount);
		return BTRFS_OK;
	}

	const uint8_t *packed;
	uint8_t *packed_copy = 0;
	uint8_t *out;
	size_t packed_size;
	bool verified = false;
	btrfs_status_t status;

	if (inline_extent) {
		out = btrfs_alloc(fs, decoded);
		if (out == 0) return BTRFS_ERR_NOMEM;

		packed = view->item + BTRFS_FILE_EXTENT_INLINE_HEADER;
		packed_size = e->inline_size;
	} else {
		if (cache->data == 0) {
			cache->data = btrfs_alloc(fs, BTRFS_MAX_COMPRESSED_EXTENT);
			if (cache->data == 0) return BTRFS_ERR_NOMEM;
		}

		cache->disk_bytenr = 0ULL;
		out = cache->data;

		packed_size = (size_t)e->disk_num_bytes;
		packed_copy = btrfs_alloc(fs, packed_size);
		if (packed_copy == 0) return BTRFS_ERR_NOMEM;

		status = btrfs_read_data(fs, inode, e->disk_bytenr, packed_copy, packed_size, &verified);
		if (status != BTRFS_OK) {
			btrfs_free(fs, packed_copy);
			return status;
		}
		packed = packed_copy;
	}

	status = fs->decompressors[e->compression].decompress(fs->decompressors[e->compression].ctx, packed, packed_size, out, decoded);
	btrfs_free(fs, packed_copy);

	if (status != BTRFS_OK) {
		BTRFS_LOG(fs, "inode %llu: %s extent at %llu does not decode: %s", BTRFS_U64(inode->ino), btrfs_compression_name(e->compression), BTRFS_U64(e->disk_bytenr), btrfs_status_name(status));
		if (inline_extent) btrfs_free(fs, out);
		return status == BTRFS_ERR_NOMEM ? status : BTRFS_ERR_CORRUPT;
	}

	fs->stats.extents_decoded++;
	memcpy(buffer, out + (size_t)start, amount);

	if (inline_extent) {
		btrfs_free(fs, out);
	} else {
		cache->disk_bytenr = e->disk_bytenr;
		cache->disk_num_bytes = e->disk_num_bytes;
		cache->ram_bytes = e->ram_bytes;
		cache->compression = e->compression;
		cache->verified = verified;
	}

	return BTRFS_OK;
}

/*
 * Copy `amount` bytes of the file starting `skip` bytes into this extent.
 */
static btrfs_status_t btrfs_read_extent(btrfs_fs_t *fs, const btrfs_inode_t *inode, const btrfs_extent_view_t *view, uint64_t skip, uint8_t *buffer, size_t amount)
{
	const btrfs_file_extent_t *e = &view->extent;

	if (e->encryption != 0U || e->other_encoding != 0U) return BTRFS_ERR_UNSUPPORTED_ENCRYPTION;

	if (e->type == BTRFS_FILE_EXTENT_INLINE) {
		if (e->compression != BTRFS_COMPRESS_NONE) return btrfs_read_compressed(fs, inode, view, skip, buffer, amount);
		if (skip > e->inline_size || amount > e->inline_size - skip) return BTRFS_ERR_CORRUPT;
		memcpy(buffer, view->item + BTRFS_FILE_EXTENT_INLINE_HEADER + skip, amount);
		return BTRFS_OK;
	}

	/* Preallocated space and explicit holes read as zeros. */
	if (e->type == BTRFS_FILE_EXTENT_PREALLOC || e->disk_bytenr == 0ULL) {
		memset(buffer, 0, amount);
		return BTRFS_OK;
	}

	if (e->compression != BTRFS_COMPRESS_NONE) return btrfs_read_compressed(fs, inode, view, skip, buffer, amount);

	uint64_t logical = e->disk_bytenr + e->offset + skip;

	bool verified;

	return btrfs_read_data(fs, inode, logical, buffer, amount, &verified);
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
