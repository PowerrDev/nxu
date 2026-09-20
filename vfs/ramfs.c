#include <vfs/ramfs.h>
#include <kern/machine/cpu.h>

#include <kern/memory/heap.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define RAMFS_FILE_INITIAL_CAPACITY 256ULL
#define RAMFS_FILE_MAX_SIZE (1024ULL * 1024ULL)

typedef struct ramfs_node ramfs_node_t;

typedef struct {
	ramfs_node_t *root;
	uint64_t next_node_id;
	uint32_t node_count;
	volatile uint32_t lock;
} ramfs_mount_data_t;

struct ramfs_node {
	struct vnode vnode;
	ramfs_node_t *parent;
	ramfs_node_t *children;
	ramfs_node_t *next_sibling;
	char name[VFS_NAME_MAX + 1U];
	uint8_t *data;
	uint64_t capacity;
};

static vfs_status_t ramfs_mount(filesystem_t filesystem, block_device_t device, mount_t mount);
static vfs_status_t ramfs_sync(mount_t mount);
static vfs_status_t ramfs_unmount(mount_t mount);
static vfs_status_t ramfs_lookup(vnode_t directory, const char *name, vnode_t *result);
static vfs_status_t ramfs_create(vnode_t directory, const char *name, vnode_type_t type, vnode_t *result);
static vfs_status_t ramfs_unlink(vnode_t directory, const char *name);
static vfs_status_t ramfs_readdir(vnode_t directory, uint64_t *offset, vfs_dirent_t *entry);
static vfs_status_t ramfs_read(vnode_t vnode, uint64_t offset, void *buffer, uint64_t size, uint64_t *read_size);
static vfs_status_t ramfs_write(vnode_t vnode, uint64_t offset, const void *buffer, uint64_t size, uint64_t *written_size);
static vfs_status_t ramfs_truncate(vnode_t vnode, uint64_t size);

static const filesystem_operations_t g_ramfs_filesystem_ops = {
	.mount = ramfs_mount,
	.sync = ramfs_sync,
	.unmount = ramfs_unmount
};

static const vnode_operations_t g_ramfs_vnode_ops = {
	.lookup = ramfs_lookup,
	.create = ramfs_create,
	.unlink = ramfs_unlink,
	.readdir = ramfs_readdir,
	.read = ramfs_read,
	.write = ramfs_write,
	.truncate = ramfs_truncate
};

static struct vfs_filesystem g_ramfs_filesystem = {
	.fs_name = "ramfs",
	.fs_ops = &g_ramfs_filesystem_ops,
	.fs_registered = false
};

static void ramfs_lock(ramfs_mount_data_t *data)
{
	while (__atomic_exchange_n(&data->lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
		cpu_relax();
	}
}

static void ramfs_unlock(ramfs_mount_data_t *data)
{
	__atomic_store_n(&data->lock, 0U, __ATOMIC_RELEASE);
}

static bool ramfs_copy_name(char destination[VFS_NAME_MAX + 1U], const char *source)
{
	uint32_t index = 0U;
	while (index <= VFS_NAME_MAX && source[index] != '\0') {
		if (index == VFS_NAME_MAX) return false;
		destination[index] = source[index];
		index++;
	}
	destination[index] = '\0';
	return true;
}

static ramfs_mount_data_t *ramfs_mount_data(vnode_t vnode)
{
	if (vnode == 0 || vnode->v_mount == 0) return 0;
	return vnode->v_mount->m_data;
}

static ramfs_node_t *ramfs_node(vnode_t vnode)
{
	if (vnode == 0) return 0;
	return vnode->v_data;
}

/*
 * ramfs_allocate_node:
 *
 * Allocate one resident filesystem node. The vnode's initial reference is
 * the filesystem residency reference and remains held until unmount.
 */
static ramfs_node_t *ramfs_allocate_node(
	ramfs_mount_data_t *data,
	mount_t mount,
	ramfs_node_t *parent,
	const char *name,
	vnode_type_t type
)
{
	ramfs_node_t *node = kcalloc(1U, sizeof(*node));
	if (node == 0) return 0;

	if (!ramfs_copy_name(node->name, name)) {
		(void)kfree(node);
		return 0;
	}

	node->parent = parent;

	vnode_init(
		&node->vnode,
		mount,
		parent != 0 ? &parent->vnode : 0,
		&g_ramfs_vnode_ops,
		type,
		data->next_node_id++,
		node
	);

	data->node_count++;
	return node;
}

static void ramfs_free_tree(ramfs_node_t *node)
{
	if (node == 0) return;

	ramfs_node_t *child = node->children;
	while (child != 0) {
		ramfs_node_t *next = child->next_sibling;
		ramfs_free_tree(child);
		child = next;
	}

	if (node->data != 0) (void)kfree(node->data);
	(void)kfree(node);
}

/*
 * ramfs_reserve:
 *
 * Ensure that a regular file can represent required bytes. Growth is
 * geometric so sequential writes do not allocate for every byte range.
 */
static vfs_status_t ramfs_reserve(ramfs_node_t *node, uint64_t required)
{
	if (required <= node->capacity) return VFS_STATUS_OK;

	if (required > RAMFS_FILE_MAX_SIZE) return VFS_STATUS_NO_SPACE;

	uint64_t capacity = node->capacity == 0ULL ? RAMFS_FILE_INITIAL_CAPACITY : node->capacity;
	while (capacity < required) {
		uint64_t next = capacity * 2ULL;
		if (next < capacity || next > RAMFS_FILE_MAX_SIZE) {
			capacity = RAMFS_FILE_MAX_SIZE;
			break;
		}
		capacity = next;
	}

	if (capacity < required) return VFS_STATUS_NO_SPACE;

	uint8_t *storage = kmalloc((size_t)capacity);
	if (storage == 0) return VFS_STATUS_NO_MEMORY;

	memset(storage, 0, (size_t)capacity);
	if (node->data != 0 && node->vnode.v_size != 0ULL) {
		memcpy(storage, node->data, (size_t)node->vnode.v_size);
		(void)kfree(node->data);
	}

	node->data = storage;
	node->capacity = capacity;
	return VFS_STATUS_OK;
}

static vfs_status_t ramfs_mount(filesystem_t filesystem, block_device_t device, mount_t mount)
{
	(void)filesystem;

	if (mount == 0) return VFS_STATUS_INVALID;

	if (device != 0) return VFS_STATUS_INVALID;

	ramfs_mount_data_t *data = kcalloc(1U, sizeof(*data));
	if (data == 0) return VFS_STATUS_NO_MEMORY;

	data->next_node_id = 1ULL;

	ramfs_node_t *root = ramfs_allocate_node(data, mount, 0, "", VNODE_TYPE_DIRECTORY);
	if (root == 0) {
		(void)kfree(data);
		return VFS_STATUS_NO_MEMORY;
	}

	data->root = root;
	mount->m_data = data;
	mount->m_root = &root->vnode;
	return VFS_STATUS_OK;
}

static vfs_status_t ramfs_sync(mount_t mount)
{
	if (mount == 0 || mount->m_data == 0) return VFS_STATUS_INVALID;
	return VFS_STATUS_OK;
}

static vfs_status_t ramfs_unmount(mount_t mount)
{
	if (mount == 0 || mount->m_data == 0) return VFS_STATUS_INVALID;

	ramfs_mount_data_t *data = mount->m_data;
	if (data->root != 0 && data->root->vnode.v_refcount != 1U) return VFS_STATUS_BUSY;

	ramfs_free_tree(data->root);
	(void)kfree(data);
	mount->m_data = 0;
	mount->m_root = 0;
	return VFS_STATUS_OK;
}

static vfs_status_t ramfs_lookup(vnode_t directory, const char *name, vnode_t *result)
{
	*result = 0;

	ramfs_node_t *directory_node = ramfs_node(directory);
	ramfs_mount_data_t *data = ramfs_mount_data(directory);
	if (directory_node == 0 || data == 0) return VFS_STATUS_INVALID;

	ramfs_lock(data);

	for (ramfs_node_t *child = directory_node->children; child != 0; child = child->next_sibling) {
		uint32_t index = 0U;
		while (index <= VFS_NAME_MAX && child->name[index] == name[index]) {
			if (name[index] == '\0') {
				if (!vnode_reference(&child->vnode)) {
					ramfs_unlock(data);
					return VFS_STATUS_IO_ERROR;
				}
				*result = &child->vnode;
				ramfs_unlock(data);
				return VFS_STATUS_OK;
			}
			index++;
		}
	}

	ramfs_unlock(data);
	return VFS_STATUS_NOT_FOUND;
}

/*
 * Routine:     ramfs_readdir
 * Purpose:
 *              Enumerate resident child nodes by sibling index. The index is
 *              retained in the caller's open-file offset.
 */
static vfs_status_t
ramfs_readdir(vnode_t directory, uint64_t *offset, vfs_dirent_t *entry)
{
	if (directory == 0 || offset == 0 || entry == 0) return VFS_STATUS_INVALID;

	ramfs_node_t *directory_node = ramfs_node(directory);
	ramfs_mount_data_t *data = ramfs_mount_data(directory);
	if (directory_node == 0 || data == 0) return VFS_STATUS_INVALID;

	ramfs_lock(data);

	ramfs_node_t *child = directory_node->children;
	uint64_t index = 0ULL;
	while (child != 0 && index < *offset) {
		child = child->next_sibling;
		index++;
	}

	if (child == 0) {
		ramfs_unlock(data);
		return VFS_STATUS_END_OF_DIRECTORY;
	}

	uint32_t name_length = 0U;
	while (name_length < VFS_DIRENT_NAME_MAX && child->name[name_length] != '\0') {
		entry->name[name_length] = child->name[name_length];
		name_length++;
	}
	entry->name[name_length] = '\0';
	entry->inode = child->vnode.v_id;
	entry->type = child->vnode.v_type;
	entry->name_length = name_length;
	(*offset)++;

	ramfs_unlock(data);
	return VFS_STATUS_OK;
}

static vfs_status_t ramfs_create(vnode_t directory, const char *name, vnode_type_t type, vnode_t *result)
{
	*result = 0;
	if (type != VNODE_TYPE_REGULAR && type != VNODE_TYPE_DIRECTORY) return VFS_STATUS_NOT_SUPPORTED;

	ramfs_node_t *directory_node = ramfs_node(directory);
	ramfs_mount_data_t *data = ramfs_mount_data(directory);
	if (directory_node == 0 || data == 0) return VFS_STATUS_INVALID;

	ramfs_lock(data);

	for (ramfs_node_t *child = directory_node->children; child != 0; child = child->next_sibling) {
		uint32_t index = 0U;
		while (index <= VFS_NAME_MAX && child->name[index] == name[index]) {
			if (name[index] == '\0') {
				ramfs_unlock(data);
				return VFS_STATUS_EXISTS;
			}
			index++;
		}
	}

	ramfs_node_t *node = ramfs_allocate_node(data, directory->v_mount, directory_node, name, type);
	if (node == 0) {
		ramfs_unlock(data);
		return VFS_STATUS_NO_MEMORY;
	}

	node->next_sibling = directory_node->children;
	directory_node->children = node;

	if (!vnode_reference(&node->vnode)) {
		ramfs_unlock(data);
		return VFS_STATUS_IO_ERROR;
	}

	*result = &node->vnode;
	ramfs_unlock(data);
	return VFS_STATUS_OK;
}

/*
 * ramfs_unlink:
 *
 * Remove one regular file from a directory. An open vnode is reported busy;
 * deferred deletion is left to a later general VFS lifetime lesson.
 */
static vfs_status_t ramfs_unlink(vnode_t directory, const char *name)
{
	ramfs_node_t *directory_node = ramfs_node(directory);
	ramfs_mount_data_t *data = ramfs_mount_data(directory);
	if (directory_node == 0 || data == 0 || name == 0) return VFS_STATUS_INVALID;

	ramfs_lock(data);

	ramfs_node_t *previous = 0;
	for (ramfs_node_t *child = directory_node->children; child != 0; child = child->next_sibling) {
		uint32_t index = 0U;
		while (index <= VFS_NAME_MAX && child->name[index] == name[index]) {
			if (name[index] == '\0') {
				if (child->vnode.v_type == VNODE_TYPE_DIRECTORY) {
					ramfs_unlock(data);
					return VFS_STATUS_IS_DIRECTORY;
				}

				if (child->vnode.v_refcount != 1U) {
					ramfs_unlock(data);
					return VFS_STATUS_BUSY;
				}

				if (previous == 0) directory_node->children = child->next_sibling;
				else previous->next_sibling = child->next_sibling;

				child->vnode.v_active = false;
				if (child->data != 0) (void)kfree(child->data);
				(void)kfree(child);
				if (data->node_count != 0U) data->node_count--;
				ramfs_unlock(data);
				return VFS_STATUS_OK;
			}
			index++;
		}
		previous = child;
	}

	ramfs_unlock(data);
	return VFS_STATUS_NOT_FOUND;
}

static vfs_status_t ramfs_read(
	vnode_t vnode,
	uint64_t offset,
	void *buffer,
	uint64_t size,
	uint64_t *read_size
)
{
	ramfs_node_t *node = ramfs_node(vnode);
	ramfs_mount_data_t *data = ramfs_mount_data(vnode);
	if (node == 0 || data == 0 || vnode->v_type != VNODE_TYPE_REGULAR) return VFS_STATUS_NOT_SUPPORTED;

	ramfs_lock(data);

	if (offset >= vnode->v_size || size == 0ULL) {
		*read_size = 0ULL;
		ramfs_unlock(data);
		return VFS_STATUS_OK;
	}

	uint64_t available = vnode->v_size - offset;
	uint64_t amount = size < available ? size : available;
	memcpy(buffer, node->data + offset, (size_t)amount);
	*read_size = amount;

	ramfs_unlock(data);
	return VFS_STATUS_OK;
}

static vfs_status_t ramfs_write(
	vnode_t vnode,
	uint64_t offset,
	const void *buffer,
	uint64_t size,
	uint64_t *written_size
)
{
	ramfs_node_t *node = ramfs_node(vnode);
	ramfs_mount_data_t *data = ramfs_mount_data(vnode);
	if (node == 0 || data == 0 || vnode->v_type != VNODE_TYPE_REGULAR) return VFS_STATUS_NOT_SUPPORTED;

	if (size == 0ULL) {
		*written_size = 0ULL;
		return VFS_STATUS_OK;
	}

	if (size > UINT64_MAX - offset) return VFS_STATUS_NO_SPACE;

	uint64_t end = offset + size;
	if (end > RAMFS_FILE_MAX_SIZE) return VFS_STATUS_NO_SPACE;

	ramfs_lock(data);

	vfs_status_t status = ramfs_reserve(node, end);
	if (status != VFS_STATUS_OK) {
		ramfs_unlock(data);
		return status;
	}

	if (offset > vnode->v_size) {
		memset(node->data + vnode->v_size, 0, (size_t)(offset - vnode->v_size));
	}

	if (size != 0ULL) memcpy(node->data + offset, buffer, (size_t)size);
	if (end > vnode->v_size) vnode->v_size = end;
	*written_size = size;

	ramfs_unlock(data);
	return VFS_STATUS_OK;
}

static vfs_status_t ramfs_truncate(vnode_t vnode, uint64_t size)
{
	ramfs_node_t *node = ramfs_node(vnode);
	ramfs_mount_data_t *data = ramfs_mount_data(vnode);
	if (node == 0 || data == 0 || vnode->v_type != VNODE_TYPE_REGULAR) return VFS_STATUS_NOT_SUPPORTED;

	if (size > RAMFS_FILE_MAX_SIZE) return VFS_STATUS_NO_SPACE;

	ramfs_lock(data);

	vfs_status_t status = ramfs_reserve(node, size);
	if (status != VFS_STATUS_OK) {
		ramfs_unlock(data);
		return status;
	}

	if (size > vnode->v_size) memset(node->data + vnode->v_size, 0, (size_t)(size - vnode->v_size));
	vnode->v_size = size;

	ramfs_unlock(data);
	return VFS_STATUS_OK;
}

bool ramfs_register(void)
{
	return vfs_register_filesystem(&g_ramfs_filesystem);
}
