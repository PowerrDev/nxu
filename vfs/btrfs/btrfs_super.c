/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_super.c
 *
 * Superblock location, validation and feature gating.
 *
 * A Btrfs device carries up to three superblock copies, each 4096 bytes:
 *
 *   primary   64 KiB
 *   mirror 1  64 MiB
 *   mirror 2  256 GiB
 *
 * A copy only counts when the device is large enough to hold it. Every copy
 * that is present is examined; the valid one with the highest generation wins
 * (all copies are written at each commit, so a lower generation only appears
 * after an interrupted commit or when a copy is damaged). A damaged primary
 * therefore falls back to a mirror instead of failing the mount, the same
 * thing btrfs-progs does. A copy is valid when
 *
 *   - its magic is "_BHRfS_M",
 *   - it names its own byte number correctly (a copy that was cloned to
 *     another offset is not a superblock of that offset),
 *   - its crc32c matches,
 *   - its geometry is sane (power-of-two sector and node sizes, tree levels
 *     below BTRFS_MAX_LEVEL, aligned root pointers, sys_chunk_array_size in
 *     range).
 */

#include "btrfs_fs.h"

#include <string.h>

/* Superblock flags the driver refuses (btrfs-image dumps, an fsid change under way). */
#define BTRFS_SUPER_FLAG_METADUMP (1ULL << 33U)
#define BTRFS_SUPER_FLAG_METADUMP_V2 (1ULL << 34U)
#define BTRFS_SUPER_FLAG_CHANGING_FSID (1ULL << 35U)
#define BTRFS_SUPER_FLAG_CHANGING_FSID_V2 (1ULL << 36U)
#define BTRFS_SUPER_FLAGS_REFUSED (BTRFS_SUPER_FLAG_METADUMP | BTRFS_SUPER_FLAG_METADUMP_V2 | BTRFS_SUPER_FLAG_CHANGING_FSID | BTRFS_SUPER_FLAG_CHANGING_FSID_V2)

static bool btrfs_is_power_of_two(uint32_t value)
{
	return value != 0U && (value & (value - 1U)) == 0U;
}

/* Byte offset of superblock copy `mirror` (0..2). */
static uint64_t btrfs_super_offset(uint32_t mirror)
{
	if (mirror == 0U) return BTRFS_SUPER_INFO_OFFSET;
	return 16384ULL << (BTRFS_SUPER_MIRROR_SHIFT * mirror);
}

btrfs_status_t btrfs_super_parse(const uint8_t *raw, uint64_t expected_bytenr, uint64_t device_size, btrfs_super_t *out)
{
	(void)device_size;

	if (btrfs_get_le64(raw + BTRFS_SB_MAGIC) != BTRFS_MAGIC) return BTRFS_ERR_BAD_MAGIC;

	memset(out, 0, sizeof(*out));
	out->csum_type = btrfs_get_le16(raw + BTRFS_SB_CSUM_TYPE);

	/* The superblock names its own checksum algorithm; a type nobody knows is damage, not a feature. */
	if (btrfs_csum_type_size(out->csum_type) == 0U) return BTRFS_ERR_CORRUPT;

	if (!btrfs_csum_matches(out->csum_type, raw + BTRFS_SB_CSUM, raw + BTRFS_CSUM_SIZE, BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE)) {
		return BTRFS_ERR_CSUM;
	}

	out->bytenr = btrfs_get_le64(raw + BTRFS_SB_BYTENR);
	if (out->bytenr != expected_bytenr) return BTRFS_ERR_CORRUPT;

	memcpy(out->csum, raw + BTRFS_SB_CSUM, BTRFS_CSUM_SIZE);
	memcpy(out->fsid, raw + BTRFS_SB_FSID, BTRFS_FSID_SIZE);
	memcpy(out->metadata_uuid, raw + BTRFS_SB_METADATA_UUID, BTRFS_FSID_SIZE);
	out->flags = btrfs_get_le64(raw + BTRFS_SB_FLAGS);
	out->generation = btrfs_get_le64(raw + BTRFS_SB_GENERATION);
	out->root = btrfs_get_le64(raw + BTRFS_SB_ROOT);
	out->chunk_root = btrfs_get_le64(raw + BTRFS_SB_CHUNK_ROOT);
	out->log_root = btrfs_get_le64(raw + BTRFS_SB_LOG_ROOT);
	out->total_bytes = btrfs_get_le64(raw + BTRFS_SB_TOTAL_BYTES);
	out->bytes_used = btrfs_get_le64(raw + BTRFS_SB_BYTES_USED);
	out->root_dir_objectid = btrfs_get_le64(raw + BTRFS_SB_ROOT_DIR_OBJECTID);
	out->num_devices = btrfs_get_le64(raw + BTRFS_SB_NUM_DEVICES);
	out->sectorsize = btrfs_get_le32(raw + BTRFS_SB_SECTORSIZE);
	out->nodesize = btrfs_get_le32(raw + BTRFS_SB_NODESIZE);
	out->stripesize = btrfs_get_le32(raw + BTRFS_SB_STRIPESIZE);
	out->sys_chunk_array_size = btrfs_get_le32(raw + BTRFS_SB_SYS_CHUNK_ARRAY_SIZE);
	out->chunk_root_generation = btrfs_get_le64(raw + BTRFS_SB_CHUNK_ROOT_GENERATION);
	out->compat_flags = btrfs_get_le64(raw + BTRFS_SB_COMPAT_FLAGS);
	out->compat_ro_flags = btrfs_get_le64(raw + BTRFS_SB_COMPAT_RO_FLAGS);
	out->incompat_flags = btrfs_get_le64(raw + BTRFS_SB_INCOMPAT_FLAGS);
	out->root_level = raw[BTRFS_SB_ROOT_LEVEL];
	out->chunk_root_level = raw[BTRFS_SB_CHUNK_ROOT_LEVEL];
	out->log_root_level = raw[BTRFS_SB_LOG_ROOT_LEVEL];
	out->dev_id = btrfs_get_le64(raw + BTRFS_SB_DEV_ITEM + BTRFS_DEV_DEVID);
	out->dev_total_bytes = btrfs_get_le64(raw + BTRFS_SB_DEV_ITEM + BTRFS_DEV_TOTAL_BYTES);
	out->dev_bytes_used = btrfs_get_le64(raw + BTRFS_SB_DEV_ITEM + BTRFS_DEV_BYTES_USED);
	memcpy(out->dev_uuid, raw + BTRFS_SB_DEV_ITEM + BTRFS_DEV_UUID, BTRFS_UUID_SIZE);
	out->cache_generation = btrfs_get_le64(raw + BTRFS_SB_CACHE_GENERATION);
	out->uuid_tree_generation = btrfs_get_le64(raw + BTRFS_SB_UUID_TREE_GENERATION);

	memcpy(out->label, raw + BTRFS_SB_LABEL, BTRFS_LABEL_SIZE);
	out->label[BTRFS_LABEL_SIZE] = '\0';
	for (uint32_t index = 0U; index < BTRFS_LABEL_SIZE; index++) {
		/* A label is a C string; keep the log line printable. */
		if (out->label[index] == '\0') break;
		if ((uint8_t)out->label[index] < 0x20U || (uint8_t)out->label[index] > 0x7EU) out->label[index] = '?';
	}

	if (out->sys_chunk_array_size > BTRFS_SYSTEM_CHUNK_ARRAY_SIZE) return BTRFS_ERR_CORRUPT;
	memcpy(out->sys_chunk_array, raw + BTRFS_SB_SYS_CHUNK_ARRAY, BTRFS_SYSTEM_CHUNK_ARRAY_SIZE);

	if (!btrfs_is_power_of_two(out->sectorsize) || out->sectorsize < BTRFS_MIN_SECTORSIZE || out->sectorsize > BTRFS_MAX_SECTORSIZE) return BTRFS_ERR_CORRUPT;
	if (!btrfs_is_power_of_two(out->nodesize) || out->nodesize < out->sectorsize || out->nodesize > BTRFS_MAX_METADATA_BLOCKSIZE) return BTRFS_ERR_CORRUPT;
	if (out->root_level >= BTRFS_MAX_LEVEL || out->chunk_root_level >= BTRFS_MAX_LEVEL) return BTRFS_ERR_CORRUPT;
	if (out->root == 0ULL || out->chunk_root == 0ULL) return BTRFS_ERR_CORRUPT;
	if ((out->root & (out->sectorsize - 1U)) != 0ULL || (out->chunk_root & (out->sectorsize - 1U)) != 0ULL) return BTRFS_ERR_CORRUPT;
	if (out->total_bytes == 0ULL || out->num_devices == 0ULL) return BTRFS_ERR_CORRUPT;

	return BTRFS_OK;
}

btrfs_status_t btrfs_super_load(const btrfs_fs_t *fs, const btrfs_reader_t *reader, btrfs_super_t *out, uint64_t *offset_used, uint32_t *copies_valid)
{
	uint8_t *raw = btrfs_alloc(fs, BTRFS_SUPER_INFO_SIZE);
	btrfs_super_t *candidate = btrfs_alloc(fs, sizeof(*candidate));
	btrfs_status_t first_error = BTRFS_ERR_BAD_MAGIC;
	bool first_error_set = false;
	bool have_best = false;
	uint32_t valid = 0U;

	if (raw == 0 || candidate == 0) {
		btrfs_free(fs, raw);
		btrfs_free(fs, candidate);
		return BTRFS_ERR_NOMEM;
	}

	for (uint32_t mirror = 0U; mirror < BTRFS_SUPER_MIRROR_MAX; mirror++) {
		uint64_t offset = btrfs_super_offset(mirror);

		/* A copy only exists when the whole 4 KiB fits on the device. */
		if (offset > reader->size || reader->size - offset < BTRFS_SUPER_INFO_SIZE) break;

		if (!btrfs_reader_read(reader, offset, raw, BTRFS_SUPER_INFO_SIZE)) {
			BTRFS_LOG(fs, "superblock copy %u at %llu: read failed", mirror, BTRFS_U64(offset));
			if (!first_error_set) { first_error = BTRFS_ERR_IO; first_error_set = true; }
			continue;
		}

		btrfs_status_t status = btrfs_super_parse(raw, offset, reader->size, candidate);
		if (status != BTRFS_OK) {
			BTRFS_LOG(fs, "superblock copy %u at %llu: %s", mirror, BTRFS_U64(offset), btrfs_status_name(status));

			/*
			 * Report the most informative failure: a checksum or feature
			 * problem beats "no magic", which is what an unwritten mirror
			 * looks like.
			 */
			if (!first_error_set || (first_error == BTRFS_ERR_BAD_MAGIC && status != BTRFS_ERR_BAD_MAGIC)) {
				first_error = status;
				first_error_set = true;
			}
			continue;
		}

		valid++;
		if (!have_best || candidate->generation > out->generation) {
			*out = *candidate;
			*offset_used = offset;
			have_best = true;
		}
	}

	btrfs_free(fs, raw);
	btrfs_free(fs, candidate);

	*copies_valid = valid;
	return have_best ? BTRFS_OK : first_error;
}

/* ---- feature names ------------------------------------------------------------------ */

typedef struct {
	uint64_t bit;
	const char *name;
} btrfs_feature_name_t;

static const btrfs_feature_name_t g_incompat_names[] = {
	{ BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF, "mixed-backref" },
	{ BTRFS_FEATURE_INCOMPAT_DEFAULT_SUBVOL, "default-subvol" },
	{ BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS, "mixed-groups" },
	{ BTRFS_FEATURE_INCOMPAT_COMPRESS_LZO, "compress-lzo" },
	{ BTRFS_FEATURE_INCOMPAT_COMPRESS_ZSTD, "compress-zstd" },
	{ BTRFS_FEATURE_INCOMPAT_BIG_METADATA, "big-metadata" },
	{ BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF, "extended-iref" },
	{ BTRFS_FEATURE_INCOMPAT_RAID56, "raid56" },
	{ BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA, "skinny-metadata" },
	{ BTRFS_FEATURE_INCOMPAT_NO_HOLES, "no-holes" },
	{ BTRFS_FEATURE_INCOMPAT_METADATA_UUID, "metadata-uuid" },
	{ BTRFS_FEATURE_INCOMPAT_RAID1C34, "raid1c34" },
	{ BTRFS_FEATURE_INCOMPAT_ZONED, "zoned" },
	{ BTRFS_FEATURE_INCOMPAT_EXTENT_TREE_V2, "extent-tree-v2" },
	{ BTRFS_FEATURE_INCOMPAT_RAID_STRIPE_TREE, "raid-stripe-tree" },
	{ BTRFS_FEATURE_INCOMPAT_SIMPLE_QUOTA, "squota" }
};

static const btrfs_feature_name_t g_compat_ro_names[] = {
	{ BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE, "free-space-tree" },
	{ BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID, "free-space-tree-valid" },
	{ BTRFS_FEATURE_COMPAT_RO_VERITY, "verity" },
	{ BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE, "block-group-tree" }
};

static size_t btrfs_append(char *out, size_t capacity, size_t length, const char *text)
{
	while (*text != '\0') {
		if (length + 1U < capacity) out[length] = *text;
		length++;
		text++;
	}
	return length;
}

static size_t btrfs_append_number(char *out, size_t capacity, size_t length, unsigned value)
{
	char digits[12];
	unsigned count = 0U;

	if (value == 0U) digits[count++] = '0';
	while (value != 0U) {
		digits[count++] = (char)('0' + value % 10U);
		value /= 10U;
	}

	char text[13];
	unsigned position = 0U;
	while (count != 0U) text[position++] = digits[--count];
	text[position] = '\0';
	return btrfs_append(out, capacity, length, text);
}

void btrfs_feature_names(uint64_t incompat, uint64_t compat_ro, char *out, size_t capacity)
{
	size_t length = 0U;
	bool first = true;
	uint64_t known_incompat = 0ULL;
	uint64_t known_compat_ro = 0ULL;

	if (capacity == 0U) return;

	for (size_t index = 0U; index < sizeof(g_incompat_names) / sizeof(g_incompat_names[0]); index++) {
		known_incompat |= g_incompat_names[index].bit;
		if ((incompat & g_incompat_names[index].bit) == 0ULL) continue;
		if (!first) length = btrfs_append(out, capacity, length, ",");
		length = btrfs_append(out, capacity, length, g_incompat_names[index].name);
		first = false;
	}

	for (size_t index = 0U; index < sizeof(g_compat_ro_names) / sizeof(g_compat_ro_names[0]); index++) {
		known_compat_ro |= g_compat_ro_names[index].bit;
		if ((compat_ro & g_compat_ro_names[index].bit) == 0ULL) continue;
		if (!first) length = btrfs_append(out, capacity, length, ",");
		length = btrfs_append(out, capacity, length, g_compat_ro_names[index].name);
		first = false;
	}

	for (unsigned bit = 0U; bit < 64U; bit++) {
		if ((incompat & ~known_incompat & (1ULL << bit)) == 0ULL) continue;
		if (!first) length = btrfs_append(out, capacity, length, ",");
		length = btrfs_append(out, capacity, length, "unknown-incompat-bit");
		length = btrfs_append_number(out, capacity, length, bit);
		first = false;
	}

	(void)known_compat_ro;
	if (first) length = btrfs_append(out, capacity, length, "none");
	out[length < capacity ? length : capacity - 1U] = '\0';
}

btrfs_status_t btrfs_super_check_support(const btrfs_fs_t *fs, const btrfs_super_t *super)
{
	char names[128];

	uint64_t unsupported = super->incompat_flags & ~BTRFS_INCOMPAT_SUPPORTED;
	if (unsupported != 0ULL) {
		btrfs_feature_names(unsupported, 0ULL, names, sizeof(names));
		BTRFS_LOG(fs, "unsupported incompat features: %s", names);
		return BTRFS_ERR_UNSUPPORTED_FEATURE;
	}

	if ((super->flags & BTRFS_SUPER_FLAGS_REFUSED) != 0ULL) {
		BTRFS_LOG(fs, "superblock flags %llx mark a metadata dump or an fsid change in progress", BTRFS_U64(super->flags));
		return BTRFS_ERR_UNSUPPORTED_FEATURE;
	}

	if (super->num_devices != 1ULL) {
		BTRFS_LOG(fs, "multi-device filesystem (%llu devices) is not supported", BTRFS_U64(super->num_devices));
		return BTRFS_ERR_UNSUPPORTED_PROFILE;
	}

	uint64_t device_bytes = super->dev_total_bytes != 0ULL ? super->dev_total_bytes : super->total_bytes;
	if (device_bytes > fs->reader.size) {
		BTRFS_LOG(fs, "filesystem needs %llu bytes but the device has %llu", BTRFS_U64(device_bytes), BTRFS_U64(fs->reader.size));
		return BTRFS_ERR_TRUNCATED;
	}

	return BTRFS_OK;
}
