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
 * Profiles: SINGLE (one stripe), DUP, RAID1, RAID1C3 and RAID1C4 (every stripe
 * is a whole copy of the chunk, on one device or several: a read that fails a
 * checksum on one copy is retried on the next), RAID0 and RAID10 (stripe_len
 * bytes on one device, then the next; RAID10 mirrors each stripe on
 * sub_stripes devices) and RAID5/6 (data stripes rotating with the row; plain
 * reads go straight to the data stripe, parity is never used). btrfs_map_logical
 * answers with the copies of the piece a logical address falls in, each as a
 * (device id, physical offset) pair, and btrfs_read_logical does the reading
 * across pieces and devices. Every stripe must be on a supplied device (see
 * btrfs_fs_open_devices) and inside it.
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

/* How many devices' worth of stripes hold one copy of the chunk's data, and how many parity stripes. */
static bool btrfs_chunk_geometry(const btrfs_chunk_t *chunk, uint32_t *data_stripes)
{
	uint64_t profile = chunk->type & BTRFS_BLOCK_GROUP_PROFILE_MASK;
	uint32_t n = chunk->num_stripes;

	switch (profile) {
	case 0ULL:
		*data_stripes = 1U;
		return n == 1U;
	case BTRFS_BLOCK_GROUP_DUP:
	case BTRFS_BLOCK_GROUP_RAID1:
		*data_stripes = 1U;
		return n == 2U;
	case BTRFS_BLOCK_GROUP_RAID1C3:
		*data_stripes = 1U;
		return n == 3U;
	case BTRFS_BLOCK_GROUP_RAID1C4:
		*data_stripes = 1U;
		return n == 4U;
	case BTRFS_BLOCK_GROUP_RAID0:
		*data_stripes = n;
		return n >= 2U;
	case BTRFS_BLOCK_GROUP_RAID10:
		*data_stripes = n / 2U;
		return n >= 4U && (n % 2U) == 0U && chunk->sub_stripes == 2U;
	case BTRFS_BLOCK_GROUP_RAID5:
		*data_stripes = n - 1U;
		return n >= 2U;
	case BTRFS_BLOCK_GROUP_RAID6:
		*data_stripes = n - 2U;
		return n >= 3U;
	default:
		return false;
	}
}

/* Reject a chunk the driver cannot or must not use. */
static btrfs_status_t btrfs_chunk_check(const btrfs_fs_t *fs, const btrfs_chunk_t *chunk)
{
	uint64_t mask = fs->sectorsize - 1U;
	uint64_t profile = chunk->type & BTRFS_BLOCK_GROUP_PROFILE_MASK;
	uint64_t known = BTRFS_BLOCK_GROUP_TYPE_MASK | BTRFS_BLOCK_GROUP_PROFILE_MASK;
	uint32_t data_stripes;

	if (chunk->length == 0ULL || chunk->logical + chunk->length < chunk->logical) return BTRFS_ERR_CORRUPT;
	if ((chunk->logical & mask) != 0ULL || (chunk->length & mask) != 0ULL) return BTRFS_ERR_CORRUPT;
	if ((chunk->type & BTRFS_BLOCK_GROUP_TYPE_MASK) == 0ULL) return BTRFS_ERR_CORRUPT;

	if ((chunk->type & ~known) != 0ULL) {
		/* REMAPPED and other newer block-group flags. */
		BTRFS_LOG(fs, "chunk at %llu has unknown type bits %llx", BTRFS_U64(chunk->logical), BTRFS_U64(chunk->type & ~known));
		return BTRFS_ERR_UNSUPPORTED_FEATURE;
	}

	if (profile != 0ULL && (profile & (profile - 1ULL)) != 0ULL) return BTRFS_ERR_CORRUPT;
	if (!btrfs_chunk_geometry(chunk, &data_stripes)) return BTRFS_ERR_CORRUPT;

	/* The striped profiles cut the chunk into stripe_len pieces, one per device in turn. */
	uint64_t per_device = chunk->length;

	if (data_stripes > 1U || profile == BTRFS_BLOCK_GROUP_RAID5 || profile == BTRFS_BLOCK_GROUP_RAID6) {
		if (chunk->stripe_len < fs->sectorsize || chunk->stripe_len > (1ULL << 30U) || (chunk->stripe_len & (chunk->stripe_len - 1ULL)) != 0ULL) return BTRFS_ERR_CORRUPT;
		if (chunk->length % ((uint64_t)data_stripes * chunk->stripe_len) != 0ULL) return BTRFS_ERR_CORRUPT;
		per_device = chunk->length / data_stripes;
	}

	for (uint32_t index = 0U; index < chunk->num_stripes; index++) {
		const btrfs_stripe_t *stripe = &chunk->stripes[index];
		const btrfs_device_t *device = btrfs_device_find(fs, stripe->devid);

		if (device == 0) {
			BTRFS_LOG(fs, "chunk at %llu has a stripe on device %llu, which was not supplied", BTRFS_U64(chunk->logical), BTRFS_U64(stripe->devid));
			return BTRFS_ERR_MISSING_DEVICE;
		}

		if (stripe->offset + per_device < stripe->offset || stripe->offset + per_device > device->reader.size) {
			BTRFS_LOG(fs, "chunk at %llu: stripe %u [%llu, +%llu) lies outside device %llu", BTRFS_U64(chunk->logical), index, BTRFS_U64(stripe->offset), BTRFS_U64(per_device), BTRFS_U64(stripe->devid));
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

	/* The device items say which devices exist; every supplied device must be one of them, by uuid. */
	btrfs_key_t first_device = btrfs_key_make(BTRFS_DEV_ITEMS_OBJECTID, BTRFS_DEV_ITEM_KEY, 0ULL);
	btrfs_status_t status = btrfs_tree_search_ge(fs, &fs->chunk_tree, &first_device, &path, 0);
	uint32_t listed = 0U;

	while (status == BTRFS_OK) {
		btrfs_key_t key;
		const uint8_t *data;
		uint32_t size;

		if (!btrfs_path_item(&path, &key, &data, &size)) {
			status = BTRFS_ERR_CORRUPT;
			break;
		}

		if (key.objectid != BTRFS_DEV_ITEMS_OBJECTID || key.type != BTRFS_DEV_ITEM_KEY) break;
		if (size < BTRFS_DEV_ITEM_SIZE) {
			status = BTRFS_ERR_CORRUPT;
			break;
		}

		const btrfs_device_t *device = btrfs_device_find(fs, btrfs_get_le64(data + BTRFS_DEV_DEVID));

		if (device == 0) {
			BTRFS_LOG(fs, "device %llu is listed by the chunk tree but was not supplied", BTRFS_U64(btrfs_get_le64(data + BTRFS_DEV_DEVID)));
			status = BTRFS_ERR_MISSING_DEVICE;
			break;
		}

		if (memcmp(device->uuid, data + BTRFS_DEV_UUID, BTRFS_UUID_SIZE) != 0) {
			BTRFS_LOG(fs, "device %llu is not the device the chunk tree lists under that id (uuid differs)", BTRFS_U64(device->devid));
			status = BTRFS_ERR_INVALID;
			break;
		}

		listed++;
		status = btrfs_path_next(fs, &path);
	}

	btrfs_path_release(fs, &path);

	if (status == BTRFS_ERR_END) status = BTRFS_OK;
	if (status != BTRFS_OK) return status;
	if (listed != fs->device_count) {
		BTRFS_LOG(fs, "the chunk tree lists %u devices, %u were supplied", listed, fs->device_count);
		return listed > fs->device_count ? BTRFS_ERR_MISSING_DEVICE : BTRFS_ERR_INVALID;
	}

	btrfs_key_t start = btrfs_key_make(BTRFS_FIRST_CHUNK_TREE_OBJECTID, BTRFS_CHUNK_ITEM_KEY, 0ULL);
	status = btrfs_tree_search_ge(fs, &fs->chunk_tree, &start, &path, 0);
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

static unsigned btrfs_chunk_shift(uint64_t power_of_two)
{
	unsigned shift = 0U;

	while ((1ULL << shift) < power_of_two) shift++;
	return shift;
}

btrfs_status_t btrfs_map_logical(const btrfs_fs_t *fs, uint64_t logical, btrfs_mapping_t *mapping)
{
	const btrfs_chunk_map_t *map = &fs->chunks;
	uint32_t position = btrfs_chunk_lower_bound(map, logical);

	if (position >= map->count || map->chunks[position].logical > logical) return BTRFS_ERR_CORRUPT;

	const btrfs_chunk_t *chunk = &map->chunks[position];
	uint64_t inside = logical - chunk->logical;
	uint64_t profile = chunk->type & BTRFS_BLOCK_GROUP_PROFILE_MASK;
	uint32_t n = chunk->num_stripes;

	mapping->type = chunk->type;

	if (profile != BTRFS_BLOCK_GROUP_RAID0 && profile != BTRFS_BLOCK_GROUP_RAID10 && profile != BTRFS_BLOCK_GROUP_RAID5 && profile != BTRFS_BLOCK_GROUP_RAID6) {
		/* SINGLE, DUP, RAID1, RAID1C3, RAID1C4: every stripe is a whole copy. */
		mapping->copies = n;
		mapping->length = chunk->length - inside;
		for (uint32_t index = 0U; index < n; index++) {
			mapping->devid[index] = chunk->stripes[index].devid;
			mapping->physical[index] = chunk->stripes[index].offset + inside;
		}

		return BTRFS_OK;
	}

	/* Striped: stripe_len bytes on one device, then the next. */
	unsigned shift = btrfs_chunk_shift(chunk->stripe_len);
	uint64_t stripe_nr = inside >> shift;
	uint64_t within = inside & (chunk->stripe_len - 1ULL);
	uint32_t first;
	uint64_t row;

	mapping->length = chunk->stripe_len - within;
	if (mapping->length > chunk->length - inside) mapping->length = chunk->length - inside;

	if (profile == BTRFS_BLOCK_GROUP_RAID0) {
		first = (uint32_t)(stripe_nr % n);
		row = stripe_nr / n;
		mapping->copies = 1U;
	} else if (profile == BTRFS_BLOCK_GROUP_RAID10) {
		uint32_t factor = n / chunk->sub_stripes;

		first = (uint32_t)(stripe_nr % factor) * chunk->sub_stripes;
		row = stripe_nr / factor;
		mapping->copies = chunk->sub_stripes;
	} else {
		/*
		 * RAID5/6: data stripes rotate with the row, parity takes the others. A plain read
		 * goes straight to the data stripe; nothing is reconstructed from parity.
		 */
		uint32_t data = n - (profile == BTRFS_BLOCK_GROUP_RAID6 ? 2U : 1U);
		uint32_t column = (uint32_t)(stripe_nr % data);

		row = stripe_nr / data;
		first = (uint32_t)((column + row) % n);
		mapping->copies = 1U;
	}

	for (uint32_t index = 0U; index < mapping->copies; index++) {
		const btrfs_stripe_t *stripe = &chunk->stripes[first + index];

		mapping->devid[index] = stripe->devid;
		mapping->physical[index] = stripe->offset + (row << shift) + within;
	}

	return BTRFS_OK;
}

btrfs_status_t btrfs_read_logical(btrfs_fs_t *fs, uint64_t logical, void *buffer, size_t length, uint32_t copy)
{
	uint8_t *out = buffer;

	/* A striped range is several pieces on different devices; a mirrored one is a single piece. */
	while (length != 0U) {
		btrfs_mapping_t mapping;
		btrfs_status_t status = btrfs_map_logical(fs, logical, &mapping);

		if (status != BTRFS_OK) return status;
		if (copy >= mapping.copies) return BTRFS_ERR_INVALID;

		const btrfs_device_t *device = btrfs_device_find(fs, mapping.devid[copy]);
		size_t piece = (uint64_t)length < mapping.length ? length : (size_t)mapping.length;

		if (device == 0) return BTRFS_ERR_MISSING_DEVICE;

		fs->stats.device_reads++;
		fs->stats.device_bytes += piece;

		if (!btrfs_reader_read(&device->reader, mapping.physical[copy], out, piece)) return BTRFS_ERR_IO;

		logical += piece;
		out += piece;
		length -= piece;
	}

	return BTRFS_OK;
}
