/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_vfs.c
 *
 * The only file of the Btrfs driver that knows about vnodes and mounts. It
 * adapts the pure core (btrfs_fs.h and the layers below it) to the VFS:
 *
 *   - registers the "btrfs" filesystem type (btrfs_register)
 *   - mount: builds a btrfs_reader_t over the block device, opens the core
 *     filesystem read-only, and publishes the root inode of the selected
 *     subvolume as the mount root
 *   - vnodes: one resident vnode per (subvolume, inode), kept until unmount
 *     like ext4's; lookup follows subvolume points into the other tree
 *   - readdir cursor: 0 is ".", 1 is "..", every larger value is the next
 *     DIR_INDEX to look at (btrfs indexes start at 2), so a cursor saved by
 *     the caller resumes exactly after the entry it was returned with
 *   - every mutating operation returns VFS_STATUS_READ_ONLY
 *
 * Failures keep their precise cause: the VFS status is coarse, so the
 * btrfs_status_t of the last mount attempt is available from
 * btrfs_last_mount_status().
 *
 * The mount lock is a spin lock taken around each core call, like ext4's.
 */

#include <vfs/btrfs/btrfs.h>

#include <vfs/btrfs/btrfs_dir.h>
#include <vfs/btrfs/btrfs_file.h>
#include <vfs/btrfs/btrfs_fs.h>
#include <vfs/btrfs/btrfs_inode.h>
#include <vfs/btrfs/btrfs_io_block.h>
#include <vfs/btrfs/btrfs_root.h>
#include <vfs/btrfs/btrfs_tree.h>

#include <kern/console/console.h>
#include <kern/memory/heap.h>
#include <kern/machine/cpu.h>
#include <vfs/vfs.h>
#include <vfs/vnode.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BTRFS_MOUNT_MAX 4U
#define BTRFS_NODE_BUCKETS 128U
#define BTRFS_NODE_MAX 65536U

typedef struct btrfs_node btrfs_node_t;
typedef struct btrfs_mount btrfs_mount_t;

struct btrfs_node {
	struct vnode vnode;
	btrfs_tree_t tree;         /* the subvolume the inode lives in */
	btrfs_inode_t inode;
	bool synthetic;            /* a placeholder directory: no inode item on disk */
	btrfs_node_t *hash_next;   /* the (tree, inode) index; not used by placeholders */
	btrfs_node_t *all_next;    /* every node, for teardown */
};

struct btrfs_mount {
	mount_t mount;
	block_device_t device;
	btrfs_block_reader_t block;
	btrfs_reader_t reader;
	btrfs_fs_t *fs;
	btrfs_node_t *buckets[BTRFS_NODE_BUCKETS];
	btrfs_node_t *all;
	uint32_t node_count;
	volatile uint32_t lock;
	bool active;
};

static vfs_status_t btrfs_mount_op(filesystem_t filesystem, block_device_t device, mount_t mount);
static vfs_status_t btrfs_sync_op(mount_t mount);
static vfs_status_t btrfs_unmount_op(mount_t mount);
static vfs_status_t btrfs_space_info_op(mount_t mount, vfs_space_info_t *info);

static vfs_status_t btrfs_lookup(vnode_t directory, const char *name, vnode_t *result);
static vfs_status_t btrfs_create(vnode_t directory, const char *name, vnode_type_t type, vnode_t *result);
static vfs_status_t btrfs_unlink(vnode_t directory, const char *name);
static vfs_status_t btrfs_readdir(vnode_t directory, uint64_t *offset, vfs_dirent_t *entry);
static vfs_status_t btrfs_read(vnode_t vnode, uint64_t offset, void *buffer, uint64_t size, uint64_t *read_size);
static vfs_status_t btrfs_write(vnode_t vnode, uint64_t offset, const void *buffer, uint64_t size, uint64_t *written_size);
static vfs_status_t btrfs_truncate(vnode_t vnode, uint64_t size);
static vfs_status_t btrfs_getattr(vnode_t vnode, vnode_attr_t *attr);
static vfs_status_t btrfs_readlink(vnode_t vnode, char *buffer, uint64_t capacity, uint64_t *length);

static const filesystem_operations_t g_btrfs_filesystem_operations = {
	.mount = btrfs_mount_op,
	.sync = btrfs_sync_op,
	.unmount = btrfs_unmount_op,
	.space_info = btrfs_space_info_op
};

static const vnode_operations_t g_btrfs_vnode_operations = {
	.lookup = btrfs_lookup,
	.create = btrfs_create,
	.unlink = btrfs_unlink,
	.readdir = btrfs_readdir,
	.read = btrfs_read,
	.write = btrfs_write,
	.truncate = btrfs_truncate,
	.getattr = btrfs_getattr,
	.readlink = btrfs_readlink
};

static struct vfs_filesystem g_btrfs_filesystem = {
	.fs_name = "btrfs",
	.fs_ops = &g_btrfs_filesystem_operations
};

static btrfs_mount_t *g_btrfs_mounts[BTRFS_MOUNT_MAX];
static uint32_t g_btrfs_mount_count;
static btrfs_mount_options_t g_next_options;
static bool g_next_options_set;
static btrfs_status_t g_last_mount_status = BTRFS_OK;

/* ---- environment for the core ------------------------------------------------------ */

static void *btrfs_env_alloc(void *context, size_t size)
{
	(void)context;
	return kcalloc(1U, size);
}

static void btrfs_env_release(void *context, void *pointer)
{
	(void)context;
	(void)kfree(pointer);
}

static void btrfs_env_log(void *context, const char *line)
{
	(void)context;
	kputs(line);
	kputc('\n');
}

/* ---- helpers ----------------------------------------------------------------------------- */

static void btrfs_lock(btrfs_mount_t *data)
{
	while (__atomic_exchange_n(&data->lock, 1U, __ATOMIC_ACQUIRE) != 0U) cpu_relax();
}

static void btrfs_unlock(btrfs_mount_t *data)
{
	__atomic_store_n(&data->lock, 0U, __ATOMIC_RELEASE);
}

static btrfs_mount_t *btrfs_data(vnode_t vnode)
{
	if (vnode == 0 || vnode->v_mount == 0) return 0;
	return vnode->v_mount->m_data;
}

static btrfs_node_t *btrfs_node(vnode_t vnode)
{
	return vnode == 0 ? 0 : vnode->v_data;
}

/* The VFS status set is coarse; see btrfs_last_mount_status() for the exact cause. */
static vfs_status_t btrfs_vfs_status(btrfs_status_t status)
{
	switch (status) {
	case BTRFS_OK: return VFS_STATUS_OK;
	case BTRFS_ERR_INVALID: return VFS_STATUS_INVALID;
	case BTRFS_ERR_NOMEM: return VFS_STATUS_NO_MEMORY;
	case BTRFS_ERR_BAD_MAGIC: return VFS_STATUS_INVALID;
	case BTRFS_ERR_NOT_FOUND: return VFS_STATUS_NOT_FOUND;
	case BTRFS_ERR_END: return VFS_STATUS_END_OF_DIRECTORY;
	case BTRFS_ERR_NOT_DIRECTORY: return VFS_STATUS_NOT_DIRECTORY;
	case BTRFS_ERR_IS_DIRECTORY: return VFS_STATUS_IS_DIRECTORY;
	case BTRFS_ERR_NAME_TOO_LONG: return VFS_STATUS_NAME_TOO_LONG;
	case BTRFS_ERR_UNSUPPORTED:
	case BTRFS_ERR_UNSUPPORTED_FEATURE:
	case BTRFS_ERR_UNSUPPORTED_CSUM:
	case BTRFS_ERR_UNSUPPORTED_PROFILE:
	case BTRFS_ERR_UNSUPPORTED_COMPRESSION:
	case BTRFS_ERR_UNSUPPORTED_ENCRYPTION:
	case BTRFS_ERR_LOG_TREE:
		return VFS_STATUS_NOT_SUPPORTED;
	case BTRFS_ERR_IO:
	case BTRFS_ERR_CSUM:
	case BTRFS_ERR_CORRUPT:
	case BTRFS_ERR_TRUNCATED:
	default:
		return VFS_STATUS_IO_ERROR;
	}
}

static vnode_type_t btrfs_vnode_type(uint32_t mode)
{
	switch (btrfs_inode_type(mode)) {
	case BTRFS_S_IFREG: return VNODE_TYPE_REGULAR;
	case BTRFS_S_IFDIR: return VNODE_TYPE_DIRECTORY;
	case BTRFS_S_IFLNK: return VNODE_TYPE_SYMLINK;
	case BTRFS_S_IFCHR: return VNODE_TYPE_CHARACTER;
	case BTRFS_S_IFBLK: return VNODE_TYPE_BLOCK;
	case BTRFS_S_IFIFO: return VNODE_TYPE_FIFO;
	case BTRFS_S_IFSOCK: return VNODE_TYPE_SOCKET;
	default: return VNODE_TYPE_NONE;
	}
}

static vnode_type_t btrfs_ft_type(uint8_t type)
{
	switch (type) {
	case BTRFS_FT_REG_FILE: return VNODE_TYPE_REGULAR;
	case BTRFS_FT_DIR: return VNODE_TYPE_DIRECTORY;
	case BTRFS_FT_CHRDEV: return VNODE_TYPE_CHARACTER;
	case BTRFS_FT_BLKDEV: return VNODE_TYPE_BLOCK;
	case BTRFS_FT_FIFO: return VNODE_TYPE_FIFO;
	case BTRFS_FT_SOCK: return VNODE_TYPE_SOCKET;
	case BTRFS_FT_SYMLINK: return VNODE_TYPE_SYMLINK;
	default: return VNODE_TYPE_NONE;
	}
}

static uint32_t btrfs_node_bucket(uint64_t tree_id, uint64_t ino)
{
	return (uint32_t)((ino * 0x9E3779B1ULL) ^ (tree_id * 0x85EBCA6BULL)) & (BTRFS_NODE_BUCKETS - 1U);
}

/*
 * Return a vnode for (tree, ino) with one caller reference, creating and
 * caching it on first use. `inode` is the already-read inode item.
 */
static vfs_status_t btrfs_get_node_locked(btrfs_mount_t *data, const btrfs_tree_t *tree, const btrfs_inode_t *inode, bool synthetic, vnode_t parent, vnode_t *result)
{
	*result = 0;

	uint32_t bucket = btrfs_node_bucket(tree->objectid, inode->ino);

	if (!synthetic) {
		for (btrfs_node_t *node = data->buckets[bucket]; node != 0; node = node->hash_next) {
			if (node->tree.objectid != tree->objectid || node->inode.ino != inode->ino) continue;

			if (node->vnode.v_parent == 0 && parent != 0) node->vnode.v_parent = parent;
			if (!vnode_reference(&node->vnode)) return VFS_STATUS_IO_ERROR;
			*result = &node->vnode;
			return VFS_STATUS_OK;
		}
	}

	vnode_type_t type = btrfs_vnode_type(inode->item.mode);
	if (type == VNODE_TYPE_NONE) return VFS_STATUS_NOT_SUPPORTED;
	if (data->node_count >= BTRFS_NODE_MAX) return VFS_STATUS_NO_MEMORY;

	btrfs_node_t *node = kcalloc(1U, sizeof(*node));
	if (node == 0) return VFS_STATUS_NO_MEMORY;

	node->tree = *tree;
	node->inode = *inode;
	node->synthetic = synthetic;

	vnode_init(&node->vnode, data->mount, parent, &g_btrfs_vnode_operations, type, inode->ino, node);
	node->vnode.v_size = inode->item.size;

	node->all_next = data->all;
	data->all = node;
	data->node_count++;

	if (!synthetic) {
		node->hash_next = data->buckets[bucket];
		data->buckets[bucket] = node;
	}

	if (!vnode_reference(&node->vnode)) return VFS_STATUS_IO_ERROR;
	*result = &node->vnode;
	return VFS_STATUS_OK;
}

static void btrfs_destroy_mount(btrfs_mount_t *data)
{
	btrfs_node_t *node = data->all;

	while (node != 0) {
		btrfs_node_t *next = node->all_next;
		(void)kfree(node);
		node = next;
	}

	if (data->fs != 0) btrfs_fs_close(data->fs);
	(void)kfree(data);
}

/* ---- mount ------------------------------------------------------------------------------------ */

void btrfs_set_next_mount_options(const btrfs_mount_options_t *options)
{
	if (options == 0) {
		g_next_options_set = false;
		return;
	}

	g_next_options = *options;
	g_next_options_set = true;
}

btrfs_status_t btrfs_last_mount_status(void)
{
	return g_last_mount_status;
}

const char *btrfs_last_mount_status_name(void)
{
	return btrfs_status_name(g_last_mount_status);
}

static vfs_status_t btrfs_mount_fail(btrfs_status_t status)
{
	g_last_mount_status = status;
	return btrfs_vfs_status(status);
}

static vfs_status_t btrfs_mount_op(filesystem_t filesystem, block_device_t device, mount_t mount)
{
	(void)filesystem;

	btrfs_mount_options_t options = { 0ULL, false, false };
	if (g_next_options_set) options = g_next_options;
	g_next_options_set = false;
	g_last_mount_status = BTRFS_OK;

	if (device == 0 || mount == 0 || !device->registered) return btrfs_mount_fail(BTRFS_ERR_INVALID);
	if (g_btrfs_mount_count >= BTRFS_MOUNT_MAX) return VFS_STATUS_NO_SPACE;

	btrfs_mount_t *data = kcalloc(1U, sizeof(*data));
	if (data == 0) return btrfs_mount_fail(BTRFS_ERR_NOMEM);

	data->mount = mount;
	data->device = device;
	btrfs_block_reader_init(&data->block, &data->reader, device);

	btrfs_env_t env = { data, btrfs_env_alloc, btrfs_env_release, btrfs_env_log };
	btrfs_open_options_t open_options = { options.subvol_id, options.verify_data, options.ignore_log_tree, 0U };
	btrfs_status_t status;

	data->fs = btrfs_fs_open(&env, &data->reader, &open_options, &status);
	if (data->fs == 0) {
		btrfs_destroy_mount(data);
		kprintf("btrfs_mount_op: mount refused: %s\n", btrfs_status_name(status));
		return btrfs_mount_fail(status);
	}

	btrfs_subvol_t subvol;
	btrfs_inode_t root_inode;

	status = btrfs_root_lookup(data->fs, data->fs->mount_subvol, &subvol);
	if (status == BTRFS_OK) status = btrfs_inode_read(data->fs, &subvol.tree, subvol.item.root_dirid, &root_inode);
	if (status == BTRFS_OK && btrfs_inode_type(root_inode.item.mode) != BTRFS_S_IFDIR) status = BTRFS_ERR_CORRUPT;
	if (status != BTRFS_OK) {
		kprintf("btrfs_mount_op: cannot read the root directory: %s\n", btrfs_status_name(status));
		btrfs_destroy_mount(data);
		return btrfs_mount_fail(status);
	}

	mount->m_data = data;

	vnode_t root;
	vfs_status_t vfs_status = btrfs_get_node_locked(data, &subvol.tree, &root_inode, false, 0, &root);
	if (vfs_status != VFS_STATUS_OK) {
		mount->m_data = 0;
		btrfs_destroy_mount(data);
		return btrfs_mount_fail(BTRFS_ERR_NOMEM);
	}

	mount->m_root = root;
	data->active = true;
	g_btrfs_mounts[g_btrfs_mount_count++] = data;

	btrfs_fs_describe(data->fs);
	return VFS_STATUS_OK;
}

static vfs_status_t btrfs_sync_op(mount_t mount)
{
	/* Nothing is ever dirty: the mount is read-only. */
	if (mount == 0 || mount->m_data == 0) return VFS_STATUS_INVALID;
	return VFS_STATUS_OK;
}

static vfs_status_t btrfs_space_info_op(mount_t mount, vfs_space_info_t *info)
{
	if (mount == 0 || info == 0) return VFS_STATUS_INVALID;

	btrfs_mount_t *data = mount->m_data;
	if (data == 0 || !data->active) return VFS_STATUS_INVALID;

	uint64_t total = data->fs->super.total_bytes;
	uint64_t used = data->fs->super.bytes_used;

	*info = (vfs_space_info_t) {
		.total_bytes = total,
		.free_bytes = used < total ? total - used : 0ULL,
		.block_size = data->fs->sectorsize,
		.read_only = true
	};
	return VFS_STATUS_OK;
}

/*
 * Every resident vnode holds one residency reference; the root holds one more
 * for the mount. Anything above that is a live path or open file: BUSY.
 */
static vfs_status_t btrfs_unmount_op(mount_t mount)
{
	if (mount == 0 || mount->m_data == 0 || mount->m_root == 0) return VFS_STATUS_INVALID;

	btrfs_mount_t *data = mount->m_data;

	btrfs_lock(data);
	for (btrfs_node_t *node = data->all; node != 0; node = node->all_next) {
		uint32_t expected = &node->vnode == mount->m_root ? 2U : 1U;

		if (node->vnode.v_refcount != expected) {
			btrfs_unlock(data);
			return VFS_STATUS_BUSY;
		}
	}

	for (btrfs_node_t *node = data->all; node != 0; node = node->all_next) node->vnode.v_active = false;
	data->active = false;
	btrfs_unlock(data);

	for (uint32_t index = 0U; index < g_btrfs_mount_count; index++) {
		if (g_btrfs_mounts[index] != data) continue;

		for (uint32_t move = index + 1U; move < g_btrfs_mount_count; move++) g_btrfs_mounts[move - 1U] = g_btrfs_mounts[move];
		g_btrfs_mount_count--;
		g_btrfs_mounts[g_btrfs_mount_count] = 0;
		break;
	}

	mount->m_data = 0;
	mount->m_root = 0;
	btrfs_destroy_mount(data);
	return VFS_STATUS_OK;
}

/* ---- vnode operations ----------------------------------------------------------------------------- */

/*
 * Resolve one directory entry to a vnode. A subvolume point continues in the
 * subvolume's own tree at its root directory inode; a point that has no
 * subvolume behind it (a snapshot's placeholder) is an empty directory.
 */
static vfs_status_t btrfs_entry_to_vnode_locked(btrfs_mount_t *data, btrfs_node_t *directory, const btrfs_dirent_t *entry, vnode_t *result)
{
	btrfs_fs_t *fs = data->fs;
	btrfs_inode_t inode;
	btrfs_tree_t tree = directory->tree;
	bool synthetic = false;
	btrfs_status_t status;

	if (btrfs_dirent_is_subvol(entry)) {
		btrfs_subvol_t subvol;

		status = btrfs_root_resolve_point(fs, directory->tree.objectid, directory->inode.ino, entry->name, entry->name_len, entry->location.objectid, &subvol);

		if (status == BTRFS_OK) {
			tree = subvol.tree;
			status = btrfs_inode_read(fs, &tree, subvol.item.root_dirid, &inode);
		} else if (status == BTRFS_ERR_NOT_FOUND) {
			memset(&inode, 0, sizeof(inode));
			inode.ino = 2ULL;
			inode.item.mode = BTRFS_S_IFDIR | 0755U;
			inode.item.nlink = 1U;
			synthetic = true;
			status = BTRFS_OK;
		}
	} else {
		status = btrfs_inode_read(fs, &tree, entry->location.objectid, &inode);
	}

	if (status != BTRFS_OK) return btrfs_vfs_status(status);

	return btrfs_get_node_locked(data, &tree, &inode, synthetic, &directory->vnode, result);
}

static vfs_status_t btrfs_lookup(vnode_t directory, const char *name, vnode_t *result)
{
	if (result != 0) *result = 0;
	if (directory == 0 || name == 0 || result == 0) return VFS_STATUS_INVALID;

	btrfs_mount_t *data = btrfs_data(directory);
	btrfs_node_t *node = btrfs_node(directory);
	if (data == 0 || node == 0) return VFS_STATUS_INVALID;

	size_t length = 0U;
	while (name[length] != '\0') {
		if (++length > BTRFS_NAME_LEN) return VFS_STATUS_NAME_TOO_LONG;
	}

	if (length == 0U) return VFS_STATUS_NOT_FOUND;
	if (node->synthetic) return VFS_STATUS_NOT_FOUND;

	btrfs_lock(data);

	btrfs_dirent_t entry;
	btrfs_status_t status = btrfs_dir_lookup(data->fs, &node->tree, node->inode.ino, (const uint8_t *)name, length, &entry);
	vfs_status_t vfs_status;

	if (status != BTRFS_OK) vfs_status = btrfs_vfs_status(status);
	else vfs_status = btrfs_entry_to_vnode_locked(data, node, &entry, result);

	btrfs_unlock(data);
	return vfs_status;
}

static vfs_status_t btrfs_readdir(vnode_t directory, uint64_t *offset, vfs_dirent_t *result)
{
	if (directory == 0 || offset == 0 || result == 0) return VFS_STATUS_INVALID;

	btrfs_mount_t *data = btrfs_data(directory);
	btrfs_node_t *node = btrfs_node(directory);
	if (data == 0 || node == 0) return VFS_STATUS_INVALID;

	if (*offset < 2ULL) {
		/* "." and ".." are not stored on disk; the root is its own parent. */
		bool parent = *offset == 1ULL;
		vnode_t target = parent && directory->v_parent != 0 ? directory->v_parent : directory;

		result->inode = target->v_id;
		result->type = VNODE_TYPE_DIRECTORY;
		result->name_length = parent ? 2U : 1U;
		result->name[0] = '.';
		result->name[1] = parent ? '.' : '\0';
		result->name[2] = '\0';
		(*offset)++;
		return VFS_STATUS_OK;
	}

	if (node->synthetic) return VFS_STATUS_END_OF_DIRECTORY;

	btrfs_lock(data);

	btrfs_dirent_t entry;
	uint64_t cursor = *offset;
	btrfs_status_t status = btrfs_dir_next(data->fs, &node->tree, node->inode.ino, &cursor, &entry);

	if (status == BTRFS_OK) {
		memcpy(result->name, entry.name, (size_t)entry.name_len + 1U);
		result->name_length = entry.name_len;
		result->inode = btrfs_dirent_is_subvol(&entry) ? BTRFS_FIRST_FREE_OBJECTID : entry.location.objectid;
		result->type = btrfs_ft_type(entry.type);
		*offset = cursor;
	}

	btrfs_unlock(data);
	return btrfs_vfs_status(status);
}

static vfs_status_t btrfs_read(vnode_t vnode, uint64_t offset, void *buffer, uint64_t size, uint64_t *read_size)
{
	if (read_size != 0) *read_size = 0ULL;
	if (vnode == 0 || buffer == 0 || read_size == 0) return VFS_STATUS_INVALID;

	btrfs_mount_t *data = btrfs_data(vnode);
	btrfs_node_t *node = btrfs_node(vnode);
	if (data == 0 || node == 0) return VFS_STATUS_INVALID;

	if (vnode->v_type == VNODE_TYPE_DIRECTORY) return VFS_STATUS_IS_DIRECTORY;
	if (vnode->v_type != VNODE_TYPE_REGULAR && vnode->v_type != VNODE_TYPE_SYMLINK) return VFS_STATUS_NOT_SUPPORTED;

	btrfs_lock(data);
	btrfs_status_t status = btrfs_file_read(data->fs, &node->tree, &node->inode, offset, buffer, size, read_size);
	btrfs_unlock(data);

	if (status != BTRFS_OK) *read_size = 0ULL;
	return btrfs_vfs_status(status);
}

static vfs_status_t btrfs_getattr(vnode_t vnode, vnode_attr_t *attr)
{
	btrfs_node_t *node = btrfs_node(vnode);
	if (node == 0 || attr == 0) return VFS_STATUS_INVALID;

	const btrfs_inode_item_t *item = &node->inode.item;

	*attr = (vnode_attr_t) {
		.va_inode = node->inode.ino,
		.va_type = vnode->v_type,
		.va_mode = item->mode,
		.va_uid = item->uid,
		.va_gid = item->gid,
		.va_nlink = item->nlink,
		.va_size = item->size,
		.va_rdev = item->rdev,
		.va_atime_sec = item->atime.sec,
		.va_atime_nsec = item->atime.nsec,
		.va_mtime_sec = item->mtime.sec,
		.va_mtime_nsec = item->mtime.nsec,
		.va_ctime_sec = item->ctime.sec,
		.va_ctime_nsec = item->ctime.nsec
	};
	return VFS_STATUS_OK;
}

static vfs_status_t btrfs_readlink(vnode_t vnode, char *buffer, uint64_t capacity, uint64_t *length)
{
	if (length != 0) *length = 0ULL;
	if (vnode == 0 || buffer == 0 || length == 0) return VFS_STATUS_INVALID;

	btrfs_mount_t *data = btrfs_data(vnode);
	btrfs_node_t *node = btrfs_node(vnode);
	if (data == 0 || node == 0) return VFS_STATUS_INVALID;

	size_t produced = 0U;

	btrfs_lock(data);
	btrfs_status_t status = btrfs_file_readlink(data->fs, &node->tree, &node->inode, buffer, (size_t)capacity, &produced);
	btrfs_unlock(data);

	if (status == BTRFS_OK) *length = produced;
	return btrfs_vfs_status(status);
}

/* ---- every mutation is refused ------------------------------------------------------------------------ */

static vfs_status_t btrfs_create(vnode_t directory, const char *name, vnode_type_t type, vnode_t *result)
{
	(void)directory;
	(void)name;
	(void)type;
	if (result != 0) *result = 0;
	return VFS_STATUS_READ_ONLY;
}

static vfs_status_t btrfs_unlink(vnode_t directory, const char *name)
{
	(void)directory;
	(void)name;
	return VFS_STATUS_READ_ONLY;
}

static vfs_status_t btrfs_write(vnode_t vnode, uint64_t offset, const void *buffer, uint64_t size, uint64_t *written_size)
{
	(void)vnode;
	(void)offset;
	(void)buffer;
	(void)size;
	if (written_size != 0) *written_size = 0ULL;
	return VFS_STATUS_READ_ONLY;
}

static vfs_status_t btrfs_truncate(vnode_t vnode, uint64_t size)
{
	(void)vnode;
	(void)size;
	return VFS_STATUS_READ_ONLY;
}

/* ---- registration and diagnostics ---------------------------------------------------------------------------- */

bool btrfs_register(void)
{
	return vfs_register_filesystem(&g_btrfs_filesystem);
}

uint32_t btrfs_mount_count(void)
{
	return g_btrfs_mount_count;
}

void btrfs_dump(void)
{
	for (uint32_t index = 0U; index < g_btrfs_mount_count; index++) {
		btrfs_mount_t *data = g_btrfs_mounts[index];

		if (data == 0 || !data->active) continue;

		kprintf("btrfs_dump: mount %s, device %s, %u resident vnodes\n", data->mount->m_path, data->device->name, data->node_count);
		btrfs_fs_describe(data->fs);
	}
}
