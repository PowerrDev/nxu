/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_tree.c
 *
 * Tree blocks, the block cache, search and iteration. See btrfs_tree.h for the
 * contract and the safety argument.
 *
 * Tree block layout (nodesize bytes):
 *
 *   header (101 bytes: csum, fsid, bytenr, flags, chunk_tree_uuid,
 *           generation, owner, nritems, level)
 *   leaf:  nritems * 25-byte item headers (key, data offset, data size),
 *          growing up from the header; item data growing down from the end.
 *          Item offsets are relative to the end of the header.
 *   node:  nritems * 33-byte key pointers (key, child address, child
 *          generation).
 *
 * Verification on every device read (btrfs_block_verify):
 *   csum        crc32c over everything after the csum field
 *   bytenr      the block says it lives where it was read from
 *   fsid        matches the filesystem (metadata_uuid when that feature is on)
 *   level       exactly what the parent (or the root pointer) promised
 *   generation  exactly what the parent promised, and never newer than the
 *               superblock
 *   owner       the tree that owns the block (any subvolume for a subvolume
 *               tree: snapshots share blocks between trees)
 *   layout      leaf items sit back to back ending at the block end, never
 *               overlap the item headers, keys strictly ascend; node child
 *               pointers are sector aligned and non-zero, keys ascend
 *   first key   equal to the key the parent holds for it
 */

#include "btrfs_tree.h"

#include <string.h>

/* ---- helpers -------------------------------------------------------------------- */

static bool btrfs_is_fstree(uint64_t id)
{
	return id == BTRFS_FS_TREE_OBJECTID || (id >= BTRFS_FIRST_FREE_OBJECTID && id <= BTRFS_LAST_FREE_OBJECTID);
}

static bool btrfs_owner_matches(uint64_t tree_id, uint64_t owner)
{
	/* Subvolume trees share blocks with their snapshots, so any fs tree owns them. */
	if (btrfs_is_fstree(tree_id)) return btrfs_is_fstree(owner);
	return owner == tree_id;
}

static uint32_t btrfs_cache_bucket(uint64_t bytenr)
{
	return (uint32_t)((bytenr >> 12U) ^ (bytenr >> 20U) ^ (bytenr >> 28U)) & (BTRFS_CACHE_BUCKETS - 1U);
}

/* ---- item accessors ------------------------------------------------------------------ */

bool btrfs_leaf_item(const btrfs_block_t *leaf, uint32_t slot, btrfs_key_t *key, const uint8_t **data, uint32_t *size)
{
	if (leaf == 0 || leaf->level != 0U || slot >= leaf->nritems) return false;

	const uint8_t *item = leaf->data + BTRFS_HEADER_SIZE + (size_t)slot * BTRFS_ITEM_SIZE;
	uint32_t offset = btrfs_get_le32(item + BTRFS_DISK_KEY_SIZE);
	uint32_t length = btrfs_get_le32(item + BTRFS_DISK_KEY_SIZE + 4U);
	uint64_t room = (uint64_t)leaf->size - BTRFS_HEADER_SIZE;

	if ((uint64_t)offset + length > room) return false;

	if (key != 0) btrfs_key_read(item, key);
	if (data != 0) *data = leaf->data + BTRFS_HEADER_SIZE + offset;
	if (size != 0) *size = length;
	return true;
}

bool btrfs_node_slot(const btrfs_block_t *node, uint32_t slot, btrfs_key_t *key, uint64_t *blockptr, uint64_t *generation)
{
	if (node == 0 || node->level == 0U || slot >= node->nritems) return false;

	const uint8_t *ptr = node->data + BTRFS_HEADER_SIZE + (size_t)slot * BTRFS_KEY_PTR_SIZE;

	if (key != 0) btrfs_key_read(ptr, key);
	if (blockptr != 0) *blockptr = btrfs_get_le64(ptr + BTRFS_DISK_KEY_SIZE);
	if (generation != 0) *generation = btrfs_get_le64(ptr + BTRFS_DISK_KEY_SIZE + 8U);
	return true;
}

/* First key of a block that is known to be verified (nritems > 0). */
static void btrfs_block_first_key(const btrfs_block_t *block, btrfs_key_t *key)
{
	btrfs_key_read(block->data + BTRFS_HEADER_SIZE, key);
}

/* ---- verification --------------------------------------------------------------------- */

static btrfs_status_t btrfs_block_verify(const btrfs_fs_t *fs, btrfs_block_t *block, uint64_t bytenr, uint8_t expected_level, uint64_t expected_generation, uint64_t tree_id, bool is_root)
{
	const uint8_t *data = block->data;
	uint32_t size = fs->nodesize;

	if (btrfs_get_le32(data + BTRFS_HDR_CSUM) != btrfs_csum_crc32c(data + BTRFS_CSUM_SIZE, size - BTRFS_CSUM_SIZE)) {
		return BTRFS_ERR_CSUM;
	}

	if (btrfs_get_le64(data + BTRFS_HDR_BYTENR) != bytenr) return BTRFS_ERR_CORRUPT;
	if (memcmp(data + BTRFS_HDR_FSID, fs->header_fsid, BTRFS_FSID_SIZE) != 0) return BTRFS_ERR_CORRUPT;

	block->level = data[BTRFS_HDR_LEVEL];
	block->generation = btrfs_get_le64(data + BTRFS_HDR_GENERATION);
	block->owner = btrfs_get_le64(data + BTRFS_HDR_OWNER);
	block->nritems = btrfs_get_le32(data + BTRFS_HDR_NRITEMS);
	block->bytenr = bytenr;

	if (block->level >= BTRFS_MAX_LEVEL || block->level != expected_level) return BTRFS_ERR_CORRUPT;
	if (expected_generation != 0ULL && block->generation != expected_generation) return BTRFS_ERR_CORRUPT;
	if (block->generation > fs->super.generation) return BTRFS_ERR_CORRUPT;
	if (!btrfs_owner_matches(tree_id, block->owner)) return BTRFS_ERR_CORRUPT;

	uint64_t room = (uint64_t)size - BTRFS_HEADER_SIZE;
	btrfs_key_t previous;
	btrfs_key_t current;

	if (block->level == 0U) {
		uint64_t headers = (uint64_t)block->nritems * BTRFS_ITEM_SIZE;

		if (headers > room) return BTRFS_ERR_CORRUPT;
		if (block->nritems == 0U) return is_root ? BTRFS_OK : BTRFS_ERR_CORRUPT;

		uint64_t expected_end = room;

		for (uint32_t slot = 0U; slot < block->nritems; slot++) {
			const uint8_t *item = data + BTRFS_HEADER_SIZE + (size_t)slot * BTRFS_ITEM_SIZE;
			uint64_t offset = btrfs_get_le32(item + BTRFS_DISK_KEY_SIZE);
			uint64_t length = btrfs_get_le32(item + BTRFS_DISK_KEY_SIZE + 4U);

			/* Item data ends where the previous item's data begins, starting at the block end. */
			if (offset + length != expected_end) return BTRFS_ERR_CORRUPT;
			/* ... and never reaches back into the item headers. */
			if (offset < headers) return BTRFS_ERR_CORRUPT;
			expected_end = offset;

			btrfs_key_read(item, &current);
			if (slot != 0U && btrfs_key_cmp(&previous, &current) >= 0) return BTRFS_ERR_CORRUPT;
			previous = current;
		}

		return BTRFS_OK;
	}

	uint64_t pointers = (uint64_t)block->nritems * BTRFS_KEY_PTR_SIZE;

	if (block->nritems == 0U || pointers > room) return BTRFS_ERR_CORRUPT;

	uint64_t sector_mask = fs->sectorsize - 1U;

	for (uint32_t slot = 0U; slot < block->nritems; slot++) {
		const uint8_t *ptr = data + BTRFS_HEADER_SIZE + (size_t)slot * BTRFS_KEY_PTR_SIZE;
		uint64_t child = btrfs_get_le64(ptr + BTRFS_DISK_KEY_SIZE);

		if (child == 0ULL || (child & sector_mask) != 0ULL) return BTRFS_ERR_CORRUPT;

		btrfs_key_read(ptr, &current);
		if (slot != 0U && btrfs_key_cmp(&previous, &current) >= 0) return BTRFS_ERR_CORRUPT;
		previous = current;
	}

	return BTRFS_OK;
}

/* The checks that depend on who is asking, re-run for a cache hit. */
static btrfs_status_t btrfs_block_check_expectations(const btrfs_block_t *block, uint8_t expected_level, uint64_t expected_generation, uint64_t tree_id, const btrfs_key_t *first_key)
{
	if (block->level != expected_level) return BTRFS_ERR_CORRUPT;
	if (expected_generation != 0ULL && block->generation != expected_generation) return BTRFS_ERR_CORRUPT;
	if (!btrfs_owner_matches(tree_id, block->owner)) return BTRFS_ERR_CORRUPT;

	if (first_key != 0 && block->nritems != 0U) {
		btrfs_key_t found;
		btrfs_block_first_key(block, &found);
		if (btrfs_key_cmp(first_key, &found) != 0) return BTRFS_ERR_CORRUPT;
	}

	return BTRFS_OK;
}

/* ---- cache ------------------------------------------------------------------------------ */

void btrfs_cache_init(btrfs_fs_t *fs)
{
	uint32_t limit = fs->options.cache_blocks;

	if (limit == 0U) {
		/* About 512 KiB of tree blocks, between 12 and 48 blocks. */
		limit = (512U * 1024U) / fs->nodesize;
		if (limit < 12U) limit = 12U;
		if (limit > 48U) limit = 48U;
	}

	memset(&fs->cache, 0, sizeof(fs->cache));
	fs->cache.limit = limit;
}

static void btrfs_cache_unlink_lru(btrfs_cache_t *cache, btrfs_block_t *block)
{
	if (block->lru_newer != 0) block->lru_newer->lru_older = block->lru_older;
	else cache->newest = block->lru_older;

	if (block->lru_older != 0) block->lru_older->lru_newer = block->lru_newer;
	else cache->oldest = block->lru_newer;

	block->lru_newer = 0;
	block->lru_older = 0;
}

static void btrfs_cache_link_newest(btrfs_cache_t *cache, btrfs_block_t *block)
{
	block->lru_newer = 0;
	block->lru_older = cache->newest;
	if (cache->newest != 0) cache->newest->lru_newer = block;
	cache->newest = block;
	if (cache->oldest == 0) cache->oldest = block;
}

static void btrfs_cache_remove(btrfs_fs_t *fs, btrfs_block_t *block)
{
	btrfs_cache_t *cache = &fs->cache;
	btrfs_block_t **link = &cache->buckets[btrfs_cache_bucket(block->bytenr)];

	while (*link != 0 && *link != block) link = &(*link)->hash_next;
	if (*link == block) *link = block->hash_next;

	btrfs_cache_unlink_lru(cache, block);
	cache->count--;
	btrfs_free(fs, block->data);
	btrfs_free(fs, block);
}

/* Drop unpinned blocks, oldest first, until the cache fits its limit. */
static void btrfs_cache_trim(btrfs_fs_t *fs)
{
	btrfs_block_t *block = fs->cache.oldest;

	while (fs->cache.count > fs->cache.limit && block != 0) {
		btrfs_block_t *newer = block->lru_newer;

		if (block->refs == 0U) {
			btrfs_cache_remove(fs, block);
			fs->cache.evictions++;
		}

		block = newer;
	}
}

void btrfs_cache_release(btrfs_fs_t *fs)
{
	while (fs->cache.newest != 0) btrfs_cache_remove(fs, fs->cache.newest);
}

/* ---- block get / put --------------------------------------------------------------------- */

static btrfs_status_t btrfs_block_load(btrfs_fs_t *fs, btrfs_block_t *block, uint64_t bytenr, uint8_t expected_level, uint64_t expected_generation, uint64_t tree_id, const btrfs_key_t *first_key, bool is_root)
{
	btrfs_mapping_t mapping;
	btrfs_status_t status = btrfs_map_logical(fs, bytenr, &mapping);

	if (status != BTRFS_OK) return status;
	if (mapping.length < fs->nodesize) return BTRFS_ERR_CORRUPT; /* a block never crosses a chunk boundary */

	btrfs_status_t worst = BTRFS_ERR_IO;

	for (uint32_t copy = 0U; copy < mapping.copies; copy++) {
		status = btrfs_read_logical(fs, bytenr, block->data, fs->nodesize, copy);

		if (status == BTRFS_OK) {
			status = btrfs_block_verify(fs, block, bytenr, expected_level, expected_generation, tree_id, is_root);
			if (status == BTRFS_OK) status = btrfs_block_check_expectations(block, expected_level, expected_generation, tree_id, first_key);
			if (status == BTRFS_OK) {
				if (copy != 0U) fs->stats.mirror_fallbacks++;
				return BTRFS_OK;
			}
		}

		if (status == BTRFS_ERR_CSUM) fs->stats.csum_failures++;
		BTRFS_LOG(fs, "block %llu copy %u: %s", BTRFS_U64(bytenr), copy, btrfs_status_name(status));

		/* Keep the most telling failure: a checksum failure beats a structural one beats I/O. */
		if (worst == BTRFS_ERR_IO || status == BTRFS_ERR_CSUM || (worst != BTRFS_ERR_CSUM && status == BTRFS_ERR_CORRUPT)) worst = status;
	}

	return worst;
}

btrfs_status_t btrfs_block_get(btrfs_fs_t *fs, uint64_t bytenr, uint8_t expected_level, uint64_t expected_generation, uint64_t tree_id, const btrfs_key_t *first_key, bool is_root, btrfs_block_t **out)
{
	btrfs_cache_t *cache = &fs->cache;

	*out = 0;
	if ((bytenr & (fs->sectorsize - 1U)) != 0ULL) return BTRFS_ERR_CORRUPT;

	for (btrfs_block_t *block = cache->buckets[btrfs_cache_bucket(bytenr)]; block != 0; block = block->hash_next) {
		if (block->bytenr != bytenr) continue;

		btrfs_status_t status = btrfs_block_check_expectations(block, expected_level, expected_generation, tree_id, first_key);
		if (status != BTRFS_OK) return status;
		if (block->nritems == 0U && !is_root) return BTRFS_ERR_CORRUPT;

		btrfs_cache_unlink_lru(cache, block);
		btrfs_cache_link_newest(cache, block);
		block->refs++;
		cache->hits++;
		*out = block;
		return BTRFS_OK;
	}

	cache->misses++;

	btrfs_block_t *block = btrfs_alloc(fs, sizeof(*block));
	if (block == 0) return BTRFS_ERR_NOMEM;

	block->data = btrfs_alloc(fs, fs->nodesize);
	if (block->data == 0) {
		btrfs_free(fs, block);
		return BTRFS_ERR_NOMEM;
	}
	block->size = fs->nodesize;

	btrfs_status_t status = btrfs_block_load(fs, block, bytenr, expected_level, expected_generation, tree_id, first_key, is_root);
	if (status != BTRFS_OK) {
		btrfs_free(fs, block->data);
		btrfs_free(fs, block);
		fs->last_status = status;
		return status;
	}

	uint32_t bucket = btrfs_cache_bucket(bytenr);
	block->hash_next = cache->buckets[bucket];
	cache->buckets[bucket] = block;
	btrfs_cache_link_newest(cache, block);
	cache->count++;
	block->refs = 1U;

	btrfs_cache_trim(fs);

	*out = block;
	return BTRFS_OK;
}

void btrfs_block_put(btrfs_fs_t *fs, btrfs_block_t *block)
{
	if (block == 0) return;
	if (block->refs != 0U) block->refs--;
	btrfs_cache_trim(fs);
}

/* ---- paths ---------------------------------------------------------------------------------- */

void btrfs_path_init(btrfs_path_t *path)
{
	memset(path, 0, sizeof(*path));
}

void btrfs_path_release(btrfs_fs_t *fs, btrfs_path_t *path)
{
	for (uint32_t level = 0U; level < BTRFS_MAX_LEVEL; level++) {
		if (path->blocks[level] != 0) btrfs_block_put(fs, path->blocks[level]);
		path->blocks[level] = 0;
		path->slots[level] = 0U;
	}
	path->valid = false;
}

/* First slot of a leaf whose key is >= key (nritems when none). */
static uint32_t btrfs_leaf_lower_bound(const btrfs_block_t *leaf, const btrfs_key_t *key, bool *exact)
{
	uint32_t low = 0U;
	uint32_t high = leaf->nritems;

	*exact = false;

	while (low < high) {
		uint32_t middle = low + (high - low) / 2U;
		btrfs_key_t found;
		btrfs_key_read(leaf->data + BTRFS_HEADER_SIZE + (size_t)middle * BTRFS_ITEM_SIZE, &found);

		int order = btrfs_key_cmp(&found, key);
		if (order < 0) {
			low = middle + 1U;
		} else {
			if (order == 0) *exact = true;
			high = middle;
		}
	}

	return low;
}

/* Last slot of a node whose key is <= key (0 when key precedes them all). */
static uint32_t btrfs_node_child_slot(const btrfs_block_t *node, const btrfs_key_t *key)
{
	uint32_t low = 0U;
	uint32_t high = node->nritems;

	while (low < high) {
		uint32_t middle = low + (high - low) / 2U;
		btrfs_key_t found;
		btrfs_key_read(node->data + BTRFS_HEADER_SIZE + (size_t)middle * BTRFS_KEY_PTR_SIZE, &found);

		if (btrfs_key_cmp(&found, key) <= 0) low = middle + 1U;
		else high = middle;
	}

	return low == 0U ? 0U : low - 1U;
}

btrfs_status_t btrfs_tree_search(btrfs_fs_t *fs, const btrfs_tree_t *tree, const btrfs_key_t *key, btrfs_path_t *path, bool *exact)
{
	btrfs_path_release(fs, path);
	path->tree_id = tree->objectid;
	path->root_level = tree->level;

	btrfs_block_t *block;
	btrfs_status_t status = btrfs_block_get(fs, tree->bytenr, tree->level, tree->generation, tree->objectid, 0, true, &block);
	if (status != BTRFS_OK) return status;

	for (;;) {
		uint32_t level = block->level;
		path->blocks[level] = block;

		if (level == 0U) {
			bool found;
			path->slots[0] = btrfs_leaf_lower_bound(block, key, &found);
			if (exact != 0) *exact = found;
			path->valid = true;
			return BTRFS_OK;
		}

		uint32_t slot = btrfs_node_child_slot(block, key);
		path->slots[level] = slot;

		btrfs_key_t child_key;
		uint64_t child;
		uint64_t generation;
		if (!btrfs_node_slot(block, slot, &child_key, &child, &generation)) {
			btrfs_path_release(fs, path);
			return BTRFS_ERR_CORRUPT;
		}

		btrfs_block_t *next;
		status = btrfs_block_get(fs, child, (uint8_t)(level - 1U), generation, tree->objectid, &child_key, false, &next);
		if (status != BTRFS_OK) {
			btrfs_path_release(fs, path);
			return status;
		}

		block = next;
	}
}

/*
 * Move to the first item of the next leaf (or report the end). Used both to
 * step past the last slot of a leaf and to normalise a search that landed
 * beyond it. On error the path must be released.
 */
static btrfs_status_t btrfs_path_next_leaf(btrfs_fs_t *fs, btrfs_path_t *path, const btrfs_key_t *before, bool have_before)
{
	uint32_t level = 1U;

	while (level <= path->root_level && path->blocks[level] != 0) {
		const btrfs_block_t *node = path->blocks[level];

		if (path->slots[level] + 1U < node->nritems) {
			path->slots[level]++;

			for (uint32_t l = level; l > 0U; l--) {
				btrfs_key_t child_key;
				uint64_t child;
				uint64_t generation;
				btrfs_block_t *next;

				if (!btrfs_node_slot(path->blocks[l], path->slots[l], &child_key, &child, &generation)) return BTRFS_ERR_CORRUPT;

				btrfs_status_t status = btrfs_block_get(fs, child, (uint8_t)(l - 1U), generation, path->tree_id, &child_key, false, &next);
				if (status != BTRFS_OK) return status;

				btrfs_block_put(fs, path->blocks[l - 1U]);
				path->blocks[l - 1U] = next;
				path->slots[l - 1U] = 0U;
			}

			/* Strictly increasing keys are what bounds a walk of a corrupt tree. */
			if (have_before) {
				btrfs_key_t first;
				btrfs_block_first_key(path->blocks[0], &first);
				if (btrfs_key_cmp(before, &first) >= 0) return BTRFS_ERR_CORRUPT;
			}

			return BTRFS_OK;
		}

		level++;
	}

	path->slots[0] = path->blocks[0]->nritems;
	return BTRFS_ERR_END;
}

btrfs_status_t btrfs_path_next(btrfs_fs_t *fs, btrfs_path_t *path)
{
	if (!path->valid || path->blocks[0] == 0) return BTRFS_ERR_INVALID;

	btrfs_block_t *leaf = path->blocks[0];
	btrfs_key_t before;
	bool have_before = btrfs_leaf_item(leaf, path->slots[0], &before, 0, 0);

	if (path->slots[0] + 1U < leaf->nritems) {
		path->slots[0]++;

		btrfs_key_t after;
		if (!btrfs_leaf_item(leaf, path->slots[0], &after, 0, 0)) return BTRFS_ERR_CORRUPT;
		return BTRFS_OK;
	}

	btrfs_status_t status = btrfs_path_next_leaf(fs, path, &before, have_before);
	if (status != BTRFS_OK && status != BTRFS_ERR_END) path->valid = false;
	return status;
}

btrfs_status_t btrfs_path_prev(btrfs_fs_t *fs, btrfs_path_t *path)
{
	if (!path->valid || path->blocks[0] == 0) return BTRFS_ERR_INVALID;

	btrfs_block_t *leaf = path->blocks[0];

	if (path->slots[0] > 0U) {
		path->slots[0] = path->slots[0] > leaf->nritems ? leaf->nritems - 1U : path->slots[0] - 1U;
		return BTRFS_OK;
	}

	btrfs_key_t before;
	bool have_before = btrfs_leaf_item(leaf, 0U, &before, 0, 0);
	uint32_t level = 1U;

	while (level <= path->root_level && path->blocks[level] != 0) {
		if (path->slots[level] > 0U) {
			path->slots[level]--;

			for (uint32_t l = level; l > 0U; l--) {
				btrfs_key_t child_key;
				uint64_t child;
				uint64_t generation;
				btrfs_block_t *next;

				if (!btrfs_node_slot(path->blocks[l], path->slots[l], &child_key, &child, &generation)) {
					path->valid = false;
					return BTRFS_ERR_CORRUPT;
				}

				btrfs_status_t status = btrfs_block_get(fs, child, (uint8_t)(l - 1U), generation, path->tree_id, &child_key, false, &next);
				if (status != BTRFS_OK) {
					path->valid = false;
					return status;
				}

				btrfs_block_put(fs, path->blocks[l - 1U]);
				path->blocks[l - 1U] = next;
				path->slots[l - 1U] = next->nritems - 1U;
			}

			if (have_before) {
				btrfs_key_t last;
				if (!btrfs_leaf_item(path->blocks[0], path->slots[0], &last, 0, 0) || btrfs_key_cmp(&last, &before) >= 0) {
					path->valid = false;
					return BTRFS_ERR_CORRUPT;
				}
			}

			return BTRFS_OK;
		}

		level++;
	}

	return BTRFS_ERR_END;
}

bool btrfs_path_item(const btrfs_path_t *path, btrfs_key_t *key, const uint8_t **data, uint32_t *size)
{
	if (!path->valid || path->blocks[0] == 0) return false;
	return btrfs_leaf_item(path->blocks[0], path->slots[0], key, data, size);
}

btrfs_status_t btrfs_tree_search_ge(btrfs_fs_t *fs, const btrfs_tree_t *tree, const btrfs_key_t *key, btrfs_path_t *path, bool *exact)
{
	bool found;
	btrfs_status_t status = btrfs_tree_search(fs, tree, key, path, &found);

	if (exact != 0) *exact = found;
	if (status != BTRFS_OK) return status;

	if (path->slots[0] >= path->blocks[0]->nritems) {
		status = btrfs_path_next_leaf(fs, path, 0, false);
		if (status != BTRFS_OK) {
			if (status != BTRFS_ERR_END) path->valid = false;
			return status;
		}
		if (exact != 0) *exact = false;
	}

	return BTRFS_OK;
}

btrfs_status_t btrfs_tree_search_le(btrfs_fs_t *fs, const btrfs_tree_t *tree, const btrfs_key_t *key, btrfs_path_t *path, bool *exact)
{
	bool found;
	btrfs_status_t status = btrfs_tree_search(fs, tree, key, path, &found);

	if (exact != 0) *exact = found;
	if (status != BTRFS_OK) return status;
	if (found) return BTRFS_OK;

	if (exact != 0) *exact = false;
	return btrfs_path_prev(fs, path);
}

btrfs_status_t btrfs_tree_lookup(btrfs_fs_t *fs, const btrfs_tree_t *tree, const btrfs_key_t *key, btrfs_path_t *path)
{
	bool found;
	btrfs_status_t status = btrfs_tree_search(fs, tree, key, path, &found);

	if (status != BTRFS_OK) return status;
	if (!found) {
		btrfs_path_release(fs, path);
		return BTRFS_ERR_NOT_FOUND;
	}

	return BTRFS_OK;
}
