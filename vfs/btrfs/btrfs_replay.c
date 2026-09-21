/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_replay.c
 *
 * Loads the log tree into memory (see btrfs_replay.h for what it means). Every
 * item of a subvolume's log is copied out of the tree blocks into one growing
 * pool, so the overlay never holds a block pinned; the items stay sorted by key
 * as they were in the tree, which makes lookups binary searches. On top of that
 * two small tables are derived once, at load time:
 *
 *   dirents   the directory entries the log adds (DIR_INDEX items, plus the
 *             names of logged INODE_REF items, which Linux replays as links)
 *   gone      entries of the real tree that a logged INODE_REF no longer lists
 *             (a rename or unlink of a logged inode)
 *
 * Nothing here trusts the log more than the rest of the volume: the tree
 * blocks pass the usual verification, every item's size is checked before it
 * is parsed, the tables are capped, and anything not understood fails the
 * mount cleanly (BTRFS_ERR_LOG_TREE).
 */

#include "btrfs_replay.h"
#include "btrfs_inode.h"
#include "btrfs_root.h"
#include "btrfs_tree.h"

#include <string.h>

#define BTRFS_REPLAY_LOGS_MAX 32U

/* ---- growing arrays (the environment has no realloc) ---------------------------------------------- */

static btrfs_status_t btrfs_replay_grow(btrfs_fs_t *fs, void **array, uint32_t *capacity, uint32_t needed, size_t element, uint32_t limit)
{
	if (needed <= *capacity) return BTRFS_OK;
	if (needed > limit) return BTRFS_ERR_LOG_TREE;

	uint32_t grown = *capacity == 0U ? 64U : *capacity * 2U;
	if (grown < needed) grown = needed;
	if (grown > limit) grown = limit;

	void *bigger = btrfs_alloc(fs, (size_t)grown * element);
	if (bigger == 0) return BTRFS_ERR_NOMEM;

	if (*array != 0) {
		memcpy(bigger, *array, (size_t)*capacity * element);
		btrfs_free(fs, *array);
	}

	*array = bigger;
	*capacity = grown;
	return BTRFS_OK;
}

static btrfs_status_t btrfs_replay_append_item(btrfs_fs_t *fs, btrfs_replay_t *r, const btrfs_key_t *key, const uint8_t *bytes, uint32_t size)
{
	btrfs_status_t status = btrfs_replay_grow(fs, (void **)&r->items, &r->capacity, r->count + 1U, sizeof(*r->items), BTRFS_REPLAY_ITEMS_MAX);
	if (status != BTRFS_OK) return status;

	if ((uint64_t)r->data_size + size > BTRFS_REPLAY_BYTES_MAX) return BTRFS_ERR_LOG_TREE;

	if (r->data_size + size > r->data_capacity) {
		size_t grown = r->data_capacity == 0U ? 65536U : r->data_capacity * 2U;

		while (grown < r->data_size + size) grown *= 2U;

		uint8_t *bigger = btrfs_alloc(fs, grown);
		if (bigger == 0) return BTRFS_ERR_NOMEM;

		if (r->data != 0) {
			memcpy(bigger, r->data, r->data_size);
			btrfs_free(fs, r->data);
		}

		r->data = bigger;
		r->data_capacity = grown;
	}

	memcpy(r->data + r->data_size, bytes, size);
	r->items[r->count].key = *key;
	r->items[r->count].offset = (uint32_t)r->data_size;
	r->items[r->count].size = size;
	r->count++;
	r->data_size += size;
	return BTRFS_OK;
}

/* ---- lookups --------------------------------------------------------------------------------------- */

uint32_t btrfs_replay_lower_bound(const btrfs_replay_t *replay, const btrfs_key_t *key)
{
	uint32_t low = 0U;
	uint32_t high = replay->count;

	while (low < high) {
		uint32_t middle = low + (high - low) / 2U;

		if (btrfs_key_cmp(&replay->items[middle].key, key) < 0) low = middle + 1U;
		else high = middle;
	}

	return low;
}

const btrfs_log_item_t *btrfs_replay_item(const btrfs_replay_t *replay, const btrfs_key_t *key)
{
	uint32_t index = btrfs_replay_lower_bound(replay, key);

	if (index < replay->count && btrfs_key_cmp(&replay->items[index].key, key) == 0) return &replay->items[index];
	return 0;
}

const btrfs_replay_t *btrfs_replay_find(const btrfs_fs_t *fs, uint64_t subvol)
{
	for (const btrfs_replay_t *r = fs->replays; r != 0; r = r->next) {
		if (r->subvol == subvol) return r;
	}

	return 0;
}

const btrfs_log_dirent_t *btrfs_replay_dirent_ge(const btrfs_replay_t *replay, uint64_t dir, uint64_t index)
{
	uint32_t low = 0U;
	uint32_t high = replay->dirent_count;

	while (low < high) {
		uint32_t middle = low + (high - low) / 2U;
		const btrfs_log_dirent_t *d = &replay->dirents[middle];

		if (d->dir < dir || (d->dir == dir && d->index < index)) low = middle + 1U;
		else high = middle;
	}

	if (low < replay->dirent_count && replay->dirents[low].dir == dir) return &replay->dirents[low];
	return 0;
}

bool btrfs_replay_dir_removes(const btrfs_replay_t *replay, uint64_t dir, uint64_t index)
{
	btrfs_key_t start = btrfs_key_make(dir, BTRFS_DIR_LOG_INDEX_KEY, 0ULL);

	for (uint32_t i = btrfs_replay_lower_bound(replay, &start); i < replay->count; i++) {
		const btrfs_log_item_t *item = &replay->items[i];

		if (item->key.objectid != dir || item->key.type != BTRFS_DIR_LOG_INDEX_KEY) break;
		if (item->size < 8U) continue;

		/* The logged range [first, last]: what the fs tree has in it and the log does not repeat is gone. */
		if (index >= item->key.offset && index <= btrfs_get_le64(btrfs_replay_bytes(replay, item))) return true;
	}

	for (uint32_t i = 0U; i < replay->gone_count; i++) {
		if (replay->gone[i].dir == dir && replay->gone[i].index == index) return true;
	}

	return false;
}

bool btrfs_replay_dir_touched(const btrfs_replay_t *replay, uint64_t dir)
{
	if (btrfs_replay_dirent_ge(replay, dir, 0ULL) != 0) return true;

	btrfs_key_t start = btrfs_key_make(dir, BTRFS_DIR_LOG_INDEX_KEY, 0ULL);
	uint32_t i = btrfs_replay_lower_bound(replay, &start);

	if (i < replay->count && replay->items[i].key.objectid == dir && replay->items[i].key.type == BTRFS_DIR_LOG_INDEX_KEY) return true;

	for (i = 0U; i < replay->gone_count; i++) {
		if (replay->gone[i].dir == dir) return true;
	}

	return false;
}

const uint8_t *btrfs_replay_csum(const btrfs_fs_t *fs, uint64_t address, uint64_t *start, uint64_t *end)
{
	btrfs_key_t target = btrfs_key_make(BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, address);

	for (const btrfs_replay_t *r = fs->replays; r != 0; r = r->next) {
		/* The last csum item starting at or before the address (they sort last: objectid -10). */
		uint32_t low = r->csum_first;
		uint32_t high = r->count;

		while (low < high) {
			uint32_t middle = low + (high - low) / 2U;

			if (btrfs_key_cmp(&r->items[middle].key, &target) <= 0) low = middle + 1U;
			else high = middle;
		}

		if (low == r->csum_first) continue;

		const btrfs_log_item_t *item = &r->items[low - 1U];
		uint64_t covered = (uint64_t)(item->size / fs->csum_size) * fs->sectorsize;

		if (address < item->key.offset || address - item->key.offset >= covered) continue;

		*start = item->key.offset;
		*end = item->key.offset + covered;
		return btrfs_replay_bytes(r, item);
	}

	return 0;
}

/* ---- building the derived tables ------------------------------------------------------------ */

/* The next name of a packed INODE_REF item (index u64, name_len u16, name). */
static bool btrfs_replay_ref_next(const uint8_t *data, uint32_t size, uint32_t *position, uint64_t *index, uint32_t *name_at, uint16_t *name_len)
{
	uint32_t at = *position;

	if (size - at < 10U) return false;

	*index = btrfs_get_le64(data + at);
	*name_len = btrfs_get_le16(data + at + 8U);

	if (*name_len == 0U || *name_len > BTRFS_NAME_LEN || size - at - 10U < *name_len) return false;

	*name_at = at + 10U;
	*position = at + 10U + *name_len;
	return true;
}

static uint8_t btrfs_replay_ft(uint32_t mode)
{
	switch (btrfs_inode_type(mode)) {
	case BTRFS_S_IFREG: return BTRFS_FT_REG_FILE;
	case BTRFS_S_IFDIR: return BTRFS_FT_DIR;
	case BTRFS_S_IFCHR: return BTRFS_FT_CHRDEV;
	case BTRFS_S_IFBLK: return BTRFS_FT_BLKDEV;
	case BTRFS_S_IFIFO: return BTRFS_FT_FIFO;
	case BTRFS_S_IFSOCK: return BTRFS_FT_SOCK;
	case BTRFS_S_IFLNK: return BTRFS_FT_SYMLINK;
	default: return BTRFS_FT_UNKNOWN;
	}
}

static btrfs_status_t btrfs_replay_add_dirent(btrfs_fs_t *fs, btrfs_replay_t *r, uint32_t *capacity, uint64_t dir, uint64_t index, uint64_t ino, uint32_t name_offset, uint16_t name_len, uint8_t type)
{
	btrfs_status_t status = btrfs_replay_grow(fs, (void **)&r->dirents, capacity, r->dirent_count + 1U, sizeof(*r->dirents), BTRFS_REPLAY_DIRENTS_MAX);
	if (status != BTRFS_OK) return status;

	btrfs_log_dirent_t *d = &r->dirents[r->dirent_count++];

	d->dir = dir;
	d->index = index;
	d->ino = ino;
	d->name_offset = name_offset;
	d->name_len = name_len;
	d->type = type;
	return BTRFS_OK;
}

static btrfs_status_t btrfs_replay_add_gone(btrfs_fs_t *fs, btrfs_replay_t *r, uint32_t *capacity, uint64_t dir, uint64_t index)
{
	btrfs_status_t status = btrfs_replay_grow(fs, (void **)&r->gone, capacity, r->gone_count + 1U, sizeof(*r->gone), BTRFS_REPLAY_DIRENTS_MAX);
	if (status != BTRFS_OK) return status;

	r->gone[r->gone_count].dir = dir;
	r->gone[r->gone_count].index = index;
	r->gone_count++;
	return BTRFS_OK;
}

/* The mode of a logged or committed inode, or 0 if there is none. */
static uint32_t btrfs_replay_mode_of(btrfs_fs_t *fs, const btrfs_replay_t *r, const btrfs_tree_t *subvol, uint64_t ino)
{
	btrfs_key_t key = btrfs_key_make(ino, BTRFS_INODE_ITEM_KEY, 0ULL);
	const btrfs_log_item_t *logged = btrfs_replay_item(r, &key);
	btrfs_inode_item_t item;

	if (logged != 0 && btrfs_inode_item_parse(btrfs_replay_bytes(r, logged), logged->size, &item)) return item.mode;

	btrfs_inode_t inode;

	if (btrfs_inode_read(fs, subvol, ino, &inode) == BTRFS_OK) return inode.item.mode;
	return 0U;
}

static btrfs_status_t btrfs_replay_derive(btrfs_fs_t *fs, btrfs_replay_t *r, const btrfs_tree_t *subvol)
{
	uint32_t dirent_capacity = 0U;
	uint32_t gone_capacity = 0U;
	btrfs_status_t status;

	/* DIR_INDEX items first, so that on a clash they win over names derived from INODE_REFs. */
	for (uint32_t i = 0U; i < r->count; i++) {
		const btrfs_log_item_t *item = &r->items[i];
		btrfs_dir_item_t entry;
		size_t used;

		if (item->key.type != BTRFS_DIR_INDEX_KEY) continue;

		if (!btrfs_dir_item_parse(btrfs_replay_bytes(r, item), item->size, &entry, &used) || used != item->size) return BTRFS_ERR_CORRUPT;
		if (entry.location.type != BTRFS_INODE_ITEM_KEY) {
			BTRFS_LOG(fs, "log tree: a directory entry leads into another subvolume");
			return BTRFS_ERR_LOG_TREE;
		}

		status = btrfs_replay_add_dirent(fs, r, &dirent_capacity, item->key.objectid, item->key.offset, entry.location.objectid, (uint32_t)(item->offset + (uint32_t)(entry.name - btrfs_replay_bytes(r, item))), entry.name_len, entry.type);
		if (status != BTRFS_OK) return status;
	}

	for (uint32_t i = 0U; i < r->count; i++) {
		const btrfs_log_item_t *item = &r->items[i];

		if (item->key.type != BTRFS_INODE_REF_KEY) continue;

		uint64_t ino = item->key.objectid;
		uint64_t parent = item->key.offset;

		/* The root directory names itself ".." (index 0); that is not an entry of anything. */
		if (ino == parent) continue;

		uint32_t mode = btrfs_replay_mode_of(fs, r, subvol, ino);
		uint32_t position = 0U;
		uint64_t index;
		uint32_t name_at;
		uint16_t name_len;

		if (btrfs_replay_ft(mode) == BTRFS_FT_UNKNOWN) {
			BTRFS_LOG(fs, "log tree: inode %llu is referenced but has no inode item", BTRFS_U64(ino));
			return BTRFS_ERR_LOG_TREE;
		}

		while (position < item->size) {
			if (!btrfs_replay_ref_next(btrfs_replay_bytes(r, item), item->size, &position, &index, &name_at, &name_len)) return BTRFS_ERR_CORRUPT;

			status = btrfs_replay_add_dirent(fs, r, &dirent_capacity, parent, index, ino, item->offset + name_at, name_len, btrfs_replay_ft(mode));
			if (status != BTRFS_OK) return status;
		}

		/* Names the committed tree has for this inode under this parent that the log no longer lists are gone. */
		btrfs_path_t path;
		btrfs_key_t key = btrfs_key_make(ino, BTRFS_INODE_REF_KEY, parent);
		const uint8_t *old;
		uint32_t old_size;

		btrfs_path_init(&path);
		status = btrfs_tree_lookup(fs, subvol, &key, &path);
		if (status == BTRFS_ERR_NOT_FOUND) continue;
		if (status != BTRFS_OK) return status;

		if (!btrfs_path_item(&path, 0, &old, &old_size)) {
			btrfs_path_release(fs, &path);
			return BTRFS_ERR_CORRUPT;
		}

		uint32_t old_position = 0U;
		uint64_t old_index;
		uint32_t old_name_at;
		uint16_t old_len;

		status = BTRFS_OK;
		while (old_position < old_size && status == BTRFS_OK) {
			if (!btrfs_replay_ref_next(old, old_size, &old_position, &old_index, &old_name_at, &old_len)) {
				status = BTRFS_ERR_CORRUPT;
				break;
			}

			bool kept = false;

			position = 0U;
			while (position < item->size) {
				if (!btrfs_replay_ref_next(btrfs_replay_bytes(r, item), item->size, &position, &index, &name_at, &name_len)) break;

				if (index == old_index && name_len == old_len && memcmp(btrfs_replay_bytes(r, item) + name_at, old + old_name_at, name_len) == 0) {
					kept = true;
					break;
				}
			}

			if (!kept) status = btrfs_replay_add_gone(fs, r, &gone_capacity, parent, old_index);
		}

		btrfs_path_release(fs, &path);
		if (status != BTRFS_OK) return status;
	}

	/* Sort by (dir, index), stable, then drop clashes keeping the first (a DIR_INDEX item). */
	for (uint32_t i = 1U; i < r->dirent_count; i++) {
		btrfs_log_dirent_t held = r->dirents[i];
		uint32_t j = i;

		while (j > 0U && (r->dirents[j - 1U].dir > held.dir || (r->dirents[j - 1U].dir == held.dir && r->dirents[j - 1U].index > held.index))) {
			r->dirents[j] = r->dirents[j - 1U];
			j--;
		}

		r->dirents[j] = held;
	}

	uint32_t out = 0U;

	for (uint32_t i = 0U; i < r->dirent_count; i++) {
		if (out != 0U && r->dirents[out - 1U].dir == r->dirents[i].dir && r->dirents[out - 1U].index == r->dirents[i].index) continue;
		r->dirents[out++] = r->dirents[i];
	}

	r->dirent_count = out;
	return BTRFS_OK;
}

/* ---- loading one subvolume's log ------------------------------------------------------------- */

static bool btrfs_replay_known(const btrfs_key_t *key)
{
	switch (key->type) {
	case BTRFS_INODE_ITEM_KEY:
	case BTRFS_INODE_REF_KEY:
	case BTRFS_XATTR_ITEM_KEY:
	case BTRFS_DIR_LOG_ITEM_KEY:
	case BTRFS_DIR_LOG_INDEX_KEY:
	case BTRFS_DIR_INDEX_KEY:
	case BTRFS_EXTENT_DATA_KEY:
		return key->objectid != BTRFS_EXTENT_CSUM_OBJECTID;
	case BTRFS_EXTENT_CSUM_KEY:
		return key->objectid == BTRFS_EXTENT_CSUM_OBJECTID;
	default:
		return false;
	}
}

static void btrfs_replay_free(btrfs_fs_t *fs, btrfs_replay_t *r)
{
	btrfs_free(fs, r->items);
	btrfs_free(fs, r->data);
	btrfs_free(fs, r->dirents);
	btrfs_free(fs, r->gone);
	btrfs_free(fs, r);
}

static btrfs_status_t btrfs_replay_load(btrfs_fs_t *fs, uint64_t subvol_id, const btrfs_tree_t *log, const btrfs_tree_t *subvol)
{
	btrfs_replay_t *r = btrfs_alloc(fs, sizeof(*r));
	if (r == 0) return BTRFS_ERR_NOMEM;

	r->subvol = subvol_id;

	btrfs_path_t path;
	btrfs_key_t start = btrfs_key_make(0ULL, 0U, 0ULL);
	btrfs_status_t status;

	btrfs_path_init(&path);
	status = btrfs_tree_search_ge(fs, log, &start, &path, 0);

	while (status == BTRFS_OK) {
		btrfs_key_t key;
		const uint8_t *bytes;
		uint32_t size;

		if (!btrfs_path_item(&path, &key, &bytes, &size)) {
			status = BTRFS_ERR_CORRUPT;
			break;
		}

		if (!btrfs_replay_known(&key)) {
			BTRFS_LOG(fs, "log tree of subvolume %llu has an item this driver cannot replay: (%llu, %u, %llu)", BTRFS_U64(subvol_id), BTRFS_U64(key.objectid), (unsigned)key.type, BTRFS_U64(key.offset));
			status = BTRFS_ERR_LOG_TREE;
			break;
		}

		if (r->count != 0U && btrfs_key_cmp(&r->items[r->count - 1U].key, &key) >= 0) {
			status = BTRFS_ERR_CORRUPT;
			break;
		}

		status = btrfs_replay_append_item(fs, r, &key, bytes, size);
		if (status != BTRFS_OK) break;

		status = btrfs_path_next(fs, &path);
	}

	btrfs_path_release(fs, &path);
	if (status == BTRFS_ERR_END) status = BTRFS_OK;

	if (status == BTRFS_OK) {
		btrfs_key_t csum_start = btrfs_key_make(BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, 0ULL);

		r->csum_first = btrfs_replay_lower_bound(r, &csum_start);
		status = btrfs_replay_derive(fs, r, subvol);
	}

	if (status != BTRFS_OK) {
		btrfs_replay_free(fs, r);
		return status;
	}

	r->next = fs->replays;
	fs->replays = r;

	BTRFS_LOG(fs, "log tree of subvolume %llu: %u items, %u directory entries added, %u removed by name", BTRFS_U64(subvol_id), r->count, r->dirent_count, r->gone_count);
	return BTRFS_OK;
}

btrfs_status_t btrfs_replay_open(btrfs_fs_t *fs)
{
	if (fs->super.log_root == 0ULL || fs->options.ignore_log_tree) return BTRFS_OK;

	btrfs_tree_t log_roots;
	uint64_t ids[BTRFS_REPLAY_LOGS_MAX];
	btrfs_tree_t logs[BTRFS_REPLAY_LOGS_MAX];
	uint32_t found = 0U;

	log_roots.objectid = BTRFS_TREE_LOG_OBJECTID;
	log_roots.bytenr = fs->super.log_root;
	log_roots.generation = 0ULL;
	log_roots.level = fs->super.log_root_level;

	/* The log root tree: one ROOT_ITEM (TREE_LOG, ROOT_ITEM, subvolume id) per subvolume with a log. */
	btrfs_path_t path;
	btrfs_key_t start = btrfs_key_make(BTRFS_TREE_LOG_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0ULL);
	btrfs_status_t status;

	btrfs_path_init(&path);
	status = btrfs_tree_search_ge(fs, &log_roots, &start, &path, 0);

	while (status == BTRFS_OK) {
		btrfs_key_t key;
		const uint8_t *bytes;
		uint32_t size;
		btrfs_root_item_t root;

		if (!btrfs_path_item(&path, &key, &bytes, &size)) {
			status = BTRFS_ERR_CORRUPT;
			break;
		}

		if (key.objectid != BTRFS_TREE_LOG_OBJECTID || key.type != BTRFS_ROOT_ITEM_KEY) break;

		if (!btrfs_root_item_parse(bytes, size, &root) || found >= BTRFS_REPLAY_LOGS_MAX) {
			status = found >= BTRFS_REPLAY_LOGS_MAX ? BTRFS_ERR_LOG_TREE : BTRFS_ERR_CORRUPT;
			break;
		}

		ids[found] = key.offset;
		logs[found].objectid = BTRFS_TREE_LOG_OBJECTID;
		logs[found].bytenr = root.bytenr;
		logs[found].generation = 0ULL;
		logs[found].level = root.level;
		found++;

		status = btrfs_path_next(fs, &path);
	}

	btrfs_path_release(fs, &path);
	if (status == BTRFS_ERR_END) status = BTRFS_OK;

	for (uint32_t i = 0U; i < found && status == BTRFS_OK; i++) {
		btrfs_subvol_t subvol;

		status = btrfs_root_lookup(fs, ids[i], &subvol);

		/* A log for a subvolume that no longer exists has nothing to replay into. */
		if (status == BTRFS_ERR_NOT_FOUND) {
			status = BTRFS_OK;
			continue;
		}

		if (status == BTRFS_OK) status = btrfs_replay_load(fs, ids[i], &logs[i], &subvol.tree);
	}

	if (status != BTRFS_OK) {
		BTRFS_LOG(fs, "log tree cannot be replayed: %s", btrfs_status_name(status));
		btrfs_replay_close(fs);
		return status;
	}

	return BTRFS_OK;
}

void btrfs_replay_close(btrfs_fs_t *fs)
{
	while (fs->replays != 0) {
		btrfs_replay_t *r = fs->replays;

		fs->replays = r->next;
		btrfs_replay_free(fs, r);
	}
}
