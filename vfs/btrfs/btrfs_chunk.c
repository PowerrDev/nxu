/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_chunk.c
 *
 * The chunk map: logical address -> physical location(s).
 *
 * Btrfs addresses everything (tree blocks, file data) by *logical* byte
 * numbers. A chunk item maps a logical range [logical, logical + length) onto
 * one or more stripes, each a (device, physical offset) pair. This file
 *
 *   1. bootstraps the map from the superblock's sys_chunk_array: a packed
 *      list of (key, chunk item) for the SYSTEM chunks, just enough to read
 *      the chunk tree itself;
 *   2. walks the chunk tree and adds every CHUNK_ITEM;
 *   3. translates logical addresses for the readers.
 *
 * Supported profiles: SINGLE (one stripe) and DUP (two stripes on the one
 * device, both holding the same bytes: a read that fails a checksum on the
 * first copy is retried on the second). RAID0/1/10/5/6 and RAID1C3/4 chunks
 * are parsed and recognised but refused with BTRFS_ERR_UNSUPPORTED_PROFILE
 * when the map is built, as is any stripe on a device other than the one we
 * were given. The chunk and mapping structures keep the general stripe list,
 * so a profile only needs a case in btrfs_map_logical() (see doc/vfs/btrfs.md).
 *
 * Every number from disk is range-checked: lengths and offsets are tested for
 * unsigned overflow and for lying inside the device, chunks must not overlap,
 * and the number of chunks is bounded by the space the chunk tree can hold.
 */

#include "btrfs_fs.h"
#include "btrfs_tree.h"

#include <string.h>

const char *btrfs_profile_name(uint64_t chunk_type)
{
	uint64_t profile = chunk_type & BTRFS_BLOCK_GROUP_PROFILE_MASK;

	if (profile == 0ULL) return "single";
	if (profile == BTRFS_BLOCK_GROUP_DUP) return "dup";
	if (profile == BTRFS_BLOCK_GROUP_RAID0) return "raid0";
	if (profile == BTRFS_BLOCK_GROUP_RAID1) return "raid1";
	if (profile == BTRFS_BLOCK_GROUP_RAID10) return "raid10";
	if (profile == BTRFS_BLOCK_GROUP_RAID5) return "raid5";
	if (profile == BTRFS_BLOCK_GROUP_RAID6) return "raid6";
	if (profile == BTRFS_BLOCK_GROUP_RAID1C3) return "raid1c3";
	if (profile == BTRFS_BLOCK_GROUP_RAID1C4) return "raid1c4";
	return "mixed-profile";
}

/* Reject a chunk the driver cannot or must not use. */
static btrfs_status_t btrfs_chunk_check(const btrfs_fs_t *fs, const btrfs_chunk_t *chunk)
{
	uint64_t mask = fs->sectorsize - 1U;
	uint64_t profile = chunk->type & BTRFS_BLOCK_GROUP_PROFILE_MASK;
	uint64_t known = BTRFS_BLOCK_GROUP_TYPE_MASK | BTRFS_BLOCK_GROUP_PROFILE_MASK;

	if (chunk->length == 0ULL || chunk->logical + chunk->length < chunk->logical) return BTRFS_ERR_CORRUPT;
	if ((chunk->logical & mask) != 0ULL || (chunk->length & mask) != 0ULL) return BTRFS_ERR_CORRUPT;
	if ((chunk->type & BTRFS_BLOCK_GROUP_TYPE_MASK) == 0ULL) return BTRFS_ERR_CORRUPT;

	if ((chunk->type & ~known) != 0ULL) {
		/* REMAPPED and other newer block-group flags. */
		BTRFS_LOG(fs, "chunk at %llu has unknown type bits %llx", BTRFS_U64(chunk->logical), BTRFS_U64(chunk->type & ~known));
		return BTRFS_ERR_UNSUPPORTED_FEATURE;
	}

	if (profile != 0ULL && (profile & (profile - 1ULL)) != 0ULL) return BTRFS_ERR_CORRUPT;

	if (profile != 0ULL && profile != BTRFS_BLOCK_GROUP_DUP) {
		BTRFS_LOG(fs, "chunk at %llu uses the %s profile, which is not supported", BTRFS_U64(chunk->logical), btrfs_profile_name(chunk->type));
		return BTRFS_ERR_UNSUPPORTED_PROFILE;
	}

	uint32_t expected_stripes = profile == BTRFS_BLOCK_GROUP_DUP ? 2U : 1U;
	if (chunk->num_stripes != expected_stripes) return BTRFS_ERR_CORRUPT;

	for (uint32_t index = 0U; index < chunk->num_stripes; index++) {
		const btrfs_stripe_t *stripe = &chunk->stripes[index];

		if (stripe->devid != fs->super.dev_id) {
			BTRFS_LOG(fs, "chunk at %llu has a stripe on device %llu, we only have device %llu", BTRFS_U64(chunk->logical), BTRFS_U64(stripe->devid), BTRFS_U64(fs->super.dev_id));
			return BTRFS_ERR_UNSUPPORTED_PROFILE;
		}

		if (stripe->offset + chunk->length < stripe->offset || stripe->offset + chunk->length > fs->reader.size) {
			BTRFS_LOG(fs, "chunk at %llu: stripe %u [%llu, +%llu) lies outside the device", BTRFS_U64(chunk->logical), index, BTRFS_U64(stripe->offset), BTRFS_U64(chunk->length));
			return BTRFS_ERR_CORRUPT;
		}
	}

	return BTRFS_OK;
}

/* Index of the first chunk whose end is above logical (binary search). */
static uint32_t btrfs_chunk_lower_bound(const btrfs_chunk_map_t *map, uint64_t logical)
{
	uint32_t low = 0U;
	uint32_t high = map->count;

	while (low < high) {
		uint32_t middle = low + (high - low) / 2U;
		const btrfs_chunk_t *chunk = &map->chunks[middle];

		if (chunk->logical + chunk->length <= logical) low = middle + 1U;
		else high = middle;
	}

	return low;
}

static btrfs_status_t btrfs_chunk_insert(btrfs_fs_t *fs, const btrfs_chunk_t *chunk)
{
	btrfs_chunk_map_t *map = &fs->chunks;
	uint32_t position = btrfs_chunk_lower_bound(map, chunk->logical);

	if (position < map->count) {
		const btrfs_chunk_t *existing = &map->chunks[position];

		if (existing->logical == chunk->logical && existing->length == chunk->length && existing->type == chunk->type) {
			return BTRFS_OK; /* the chunk tree repeats a sys_chunk_array entry */
		}

		if (existing->logical < chunk->logical + chunk->length) {
			BTRFS_LOG(fs, "chunk at %llu overlaps the chunk at %llu", BTRFS_U64(chunk->logical), BTRFS_U64(existing->logical));
			return BTRFS_ERR_CORRUPT;
		}
	}

	if (map->count == map->capacity) {
		uint32_t capacity = map->capacity == 0U ? 16U : map->capacity * 2U;
		if (capacity < map->capacity) return BTRFS_ERR_NOMEM;

		btrfs_chunk_t *grown = btrfs_alloc(fs, (size_t)capacity * sizeof(*grown));
		if (grown == 0) return BTRFS_ERR_NOMEM;

		if (map->count != 0U) memcpy(grown, map->chunks, (size_t)map->count * sizeof(*grown));
		btrfs_free(fs, map->chunks);
		map->chunks = grown;
		map->capacity = capacity;
	}

	for (uint32_t index = map->count; index > position; index--) map->chunks[index] = map->chunks[index - 1U];
	map->chunks[position] = *chunk;
	map->count++;
	return BTRFS_OK;
}

void btrfs_chunks_release(btrfs_fs_t *fs)
{
	btrfs_free(fs, fs->chunks.chunks);
	fs->chunks.chunks = 0;
	fs->chunks.count = 0U;
	fs->chunks.capacity = 0U;
}

btrfs_status_t btrfs_chunks_bootstrap(btrfs_fs_t *fs)
{
	const uint8_t *array = fs->super.sys_chunk_array;
	size_t size = fs->super.sys_chunk_array_size;
	size_t position = 0U;
	uint32_t seen = 0U;

	while (position < size) {
		if (size - position < BTRFS_DISK_KEY_SIZE + BTRFS_CHUNK_SIZE) {
			BTRFS_LOG(fs, "sys_chunk_array is truncated at byte %llu", BTRFS_U64(position));
			return BTRFS_ERR_CORRUPT;
		}

		btrfs_key_t key;
		btrfs_key_read(array + position, &key);
		position += BTRFS_DISK_KEY_SIZE;

		if (key.objectid != BTRFS_FIRST_CHUNK_TREE_OBJECTID || key.type != BTRFS_CHUNK_ITEM_KEY) {
			BTRFS_LOG(fs, "sys_chunk_array holds an unexpected key (%llu, %u)", BTRFS_U64(key.objectid), (unsigned)key.type);
			return BTRFS_ERR_CORRUPT;
		}

		btrfs_chunk_t chunk;
		size_t used;
		if (!btrfs_chunk_parse(array + position, size - position, key.offset, &chunk, &used)) {
			BTRFS_LOG(fs, "sys_chunk_array chunk at byte %llu is malformed", BTRFS_U64(position));
			return BTRFS_ERR_CORRUPT;
		}

		btrfs_status_t status = btrfs_chunk_check(fs, &chunk);
		if (status != BTRFS_OK) return status;

		status = btrfs_chunk_insert(fs, &chunk);
		if (status != BTRFS_OK) return status;

		position += used;
		seen++;
	}

	if (seen == 0U) {
		BTRFS_LOG(fs, "sys_chunk_array is empty");
		return BTRFS_ERR_CORRUPT;
	}

	return BTRFS_OK;
}

btrfs_status_t btrfs_chunks_load(btrfs_fs_t *fs)
{
	fs->chunk_tree.objectid = BTRFS_CHUNK_TREE_OBJECTID;
	fs->chunk_tree.bytenr = fs->super.chunk_root;
	fs->chunk_tree.generation = fs->super.chunk_root_generation;
	fs->chunk_tree.level = fs->super.chunk_root_level;

	btrfs_path_t path;
	btrfs_path_init(&path);

	btrfs_key_t start = btrfs_key_make(BTRFS_FIRST_CHUNK_TREE_OBJECTID, BTRFS_CHUNK_ITEM_KEY, 0ULL);
	btrfs_status_t status = btrfs_tree_search_ge(fs, &fs->chunk_tree, &start, &path, 0);
	uint32_t loaded = 0U;

	while (status == BTRFS_OK) {
		btrfs_key_t key;
		const uint8_t *data;
		uint32_t size;

		if (!btrfs_path_item(&path, &key, &data, &size)) {
			status = BTRFS_ERR_CORRUPT;
			break;
		}

		/* DEV_ITEMs sort first (objectid 1); chunks are the whole tail. */
		if (key.objectid != BTRFS_FIRST_CHUNK_TREE_OBJECTID || key.type != BTRFS_CHUNK_ITEM_KEY) break;

		btrfs_chunk_t chunk;
		size_t used;
		if (!btrfs_chunk_parse(data, size, key.offset, &chunk, &used)) {
			BTRFS_LOG(fs, "chunk item at logical %llu is malformed", BTRFS_U64(key.offset));
			status = BTRFS_ERR_CORRUPT;
			break;
		}

		status = btrfs_chunk_check(fs, &chunk);
		if (status != BTRFS_OK) break;

		status = btrfs_chunk_insert(fs, &chunk);
		if (status != BTRFS_OK) break;

		loaded++;
		status = btrfs_path_next(fs, &path);
	}

	btrfs_path_release(fs, &path);

	if (status == BTRFS_ERR_END) status = BTRFS_OK;
	if (status == BTRFS_OK && loaded == 0U) {
		BTRFS_LOG(fs, "the chunk tree holds no chunks");
		status = BTRFS_ERR_CORRUPT;
	}

	return status;
}

btrfs_status_t btrfs_map_logical(const btrfs_fs_t *fs, uint64_t logical, btrfs_mapping_t *mapping)
{
	const btrfs_chunk_map_t *map = &fs->chunks;
	uint32_t position = btrfs_chunk_lower_bound(map, logical);

	if (position >= map->count || map->chunks[position].logical > logical) return BTRFS_ERR_CORRUPT;

	const btrfs_chunk_t *chunk = &map->chunks[position];
	uint64_t inside = logical - chunk->logical;
	uint64_t profile = chunk->type & BTRFS_BLOCK_GROUP_PROFILE_MASK;

	/* Extension point: RAID0/1/10/5/6 stripe arithmetic would go here. */
	if (profile != 0ULL && profile != BTRFS_BLOCK_GROUP_DUP) return BTRFS_ERR_UNSUPPORTED_PROFILE;

	mapping->copies = chunk->num_stripes;
	mapping->length = chunk->length - inside;
	mapping->type = chunk->type;
	for (uint32_t index = 0U; index < chunk->num_stripes; index++) {
		mapping->physical[index] = chunk->stripes[index].offset + inside;
	}

	return BTRFS_OK;
}

btrfs_status_t btrfs_read_logical(btrfs_fs_t *fs, uint64_t logical, void *buffer, size_t length, uint32_t copy)
{
	btrfs_mapping_t mapping;
	btrfs_status_t status = btrfs_map_logical(fs, logical, &mapping);

	if (status != BTRFS_OK) return status;
	if (copy >= mapping.copies) return BTRFS_ERR_INVALID;
	if ((uint64_t)length > mapping.length) return BTRFS_ERR_CORRUPT; /* would cross a chunk boundary */

	fs->stats.device_reads++;
	fs->stats.device_bytes += length;

	if (!btrfs_reader_read(&fs->reader, mapping.physical[copy], buffer, length)) return BTRFS_ERR_IO;
	return BTRFS_OK;
}
