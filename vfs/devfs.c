#include <vfs/devfs.h>

#include <kern/console/console.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	struct vnode vnode;
	char name[VFS_NAME_MAX + 1U];
	const devfs_operations_t *operations;
	void *context;
	bool registered;
} devfs_node_t;

static devfs_node_t g_devfs_nodes[DEVFS_DEVICE_MAX];
static uint32_t g_devfs_node_count;

/* Only one devfs is ever mounted; its root vnode lives here. */
static struct vnode g_devfs_root;
static mount_t g_devfs_mount;

static vfs_status_t devfs_mount(filesystem_t filesystem, block_device_t device, mount_t mount);
static vfs_status_t devfs_sync(mount_t mount);
static vfs_status_t devfs_unmount(mount_t mount);
static vfs_status_t devfs_lookup(vnode_t directory, const char *name, vnode_t *result);
static vfs_status_t devfs_readdir(vnode_t directory, uint64_t *offset, vfs_dirent_t *entry);
static vfs_status_t devfs_read(vnode_t vnode, uint64_t offset, void *buffer, uint64_t size, uint64_t *read_size);
static vfs_status_t devfs_write(vnode_t vnode, uint64_t offset, const void *buffer, uint64_t size, uint64_t *written_size);
static vfs_status_t devfs_open(vnode_t vnode, uint32_t flags);
static void devfs_close(vnode_t vnode, uint32_t flags);
static vfs_status_t devfs_ioctl(vnode_t vnode, uint32_t command, void *argument);

static const filesystem_operations_t g_devfs_filesystem_ops = {
	.mount = devfs_mount,
	.sync = devfs_sync,
	.unmount = devfs_unmount
};

static const vnode_operations_t g_devfs_directory_ops = {
	.lookup = devfs_lookup,
	.readdir = devfs_readdir
};

static const vnode_operations_t g_devfs_device_ops = {
	.read = devfs_read,
	.write = devfs_write,
	.open = devfs_open,
	.close = devfs_close,
	.ioctl = devfs_ioctl
};

static struct vfs_filesystem g_devfs_filesystem = {
	.fs_name = "devfs",
	.fs_ops = &g_devfs_filesystem_ops,
	.fs_registered = false
};

bool devfs_register_device(const char *name, const devfs_operations_t *operations, void *context)
{
	if (name == 0 || operations == 0 || name[0] == '\0' || g_devfs_node_count >= DEVFS_DEVICE_MAX) return false;

	size_t length = strlen(name);

	if (length > VFS_NAME_MAX) return false;

	for (size_t index = 0U; index < length; index++) {
		if (name[index] == '/') return false;
	}

	for (uint32_t index = 0U; index < g_devfs_node_count; index++) {
		if (strcmp(g_devfs_nodes[index].name, name) == 0) return false;
	}

	devfs_node_t *node = &g_devfs_nodes[g_devfs_node_count++];

	memset(node, 0, sizeof(*node));
	memcpy(node->name, name, length + 1U);
	node->operations = operations;
	node->context = context;
	node->registered = true;
	return true;
}

uint32_t devfs_device_count(void)
{
	return g_devfs_node_count;
}

const char *devfs_device_name(uint32_t index)
{
	return index < g_devfs_node_count ? g_devfs_nodes[index].name : 0;
}

static devfs_node_t *devfs_node(vnode_t vnode)
{
	return vnode != 0 && vnode->v_type == VNODE_TYPE_CHARACTER ? vnode->v_data : 0;
}

static vfs_status_t devfs_mount(filesystem_t filesystem, block_device_t device, mount_t mount)
{
	(void)filesystem;

	if (mount == 0 || device != 0 || g_devfs_mount != 0) return VFS_STATUS_INVALID;

	vnode_init(&g_devfs_root, mount, 0, &g_devfs_directory_ops, VNODE_TYPE_DIRECTORY, 1ULL, 0);
	g_devfs_mount = mount;
	mount->m_root = &g_devfs_root;
	mount->m_data = 0;
	return VFS_STATUS_OK;
}

static vfs_status_t devfs_sync(mount_t mount)
{
	(void)mount;
	return VFS_STATUS_OK;
}

static vfs_status_t devfs_unmount(mount_t mount)
{
	if (mount != g_devfs_mount) return VFS_STATUS_INVALID;

	if (g_devfs_root.v_refcount != 1U) return VFS_STATUS_BUSY;

	for (uint32_t index = 0U; index < g_devfs_node_count; index++) {
		if (g_devfs_nodes[index].vnode.v_active && g_devfs_nodes[index].vnode.v_refcount != 1U) return VFS_STATUS_BUSY;
	}

	for (uint32_t index = 0U; index < g_devfs_node_count; index++) g_devfs_nodes[index].vnode.v_active = false;

	g_devfs_root.v_active = false;
	g_devfs_mount = 0;
	mount->m_root = 0;
	return VFS_STATUS_OK;
}

static vfs_status_t devfs_lookup(vnode_t directory, const char *name, vnode_t *result)
{
	*result = 0;

	if (directory != &g_devfs_root) return VFS_STATUS_INVALID;

	for (uint32_t index = 0U; index < g_devfs_node_count; index++) {
		devfs_node_t *node = &g_devfs_nodes[index];

		if (strcmp(node->name, name) != 0) continue;

		/* The vnode is made the first time the name is looked up after the mount; it then stays resident. */
		if (!node->vnode.v_active) {
			vnode_init(&node->vnode, g_devfs_mount, &g_devfs_root, &g_devfs_device_ops, VNODE_TYPE_CHARACTER, (uint64_t)index + 2ULL, node);
		}

		if (!vnode_reference(&node->vnode)) return VFS_STATUS_IO_ERROR;

		*result = &node->vnode;
		return VFS_STATUS_OK;
	}

	return VFS_STATUS_NOT_FOUND;
}

static vfs_status_t devfs_readdir(vnode_t directory, uint64_t *offset, vfs_dirent_t *entry)
{
	if (directory != &g_devfs_root || offset == 0 || entry == 0) return VFS_STATUS_INVALID;

	if (*offset >= g_devfs_node_count) return VFS_STATUS_END_OF_DIRECTORY;

	const devfs_node_t *node = &g_devfs_nodes[*offset];

	entry->inode = *offset + 2ULL;
	entry->type = VNODE_TYPE_CHARACTER;
	entry->name_length = (uint32_t)strlen(node->name);
	memcpy(entry->name, node->name, (size_t)entry->name_length + 1U);
	(*offset)++;
	return VFS_STATUS_OK;
}

static vfs_status_t devfs_read(vnode_t vnode, uint64_t offset, void *buffer, uint64_t size, uint64_t *read_size)
{
	(void)offset;

	devfs_node_t *node = devfs_node(vnode);

	if (node == 0) return VFS_STATUS_INVALID;
	if (node->operations->read == 0) return VFS_STATUS_NOT_SUPPORTED;

	return node->operations->read(node->context, buffer, size, read_size);
}

static vfs_status_t devfs_write(vnode_t vnode, uint64_t offset, const void *buffer, uint64_t size, uint64_t *written_size)
{
	(void)offset;

	devfs_node_t *node = devfs_node(vnode);

	if (node == 0) return VFS_STATUS_INVALID;
	if (node->operations->write == 0) return VFS_STATUS_NOT_SUPPORTED;

	return node->operations->write(node->context, buffer, size, written_size);
}

static vfs_status_t devfs_open(vnode_t vnode, uint32_t flags)
{
	devfs_node_t *node = devfs_node(vnode);

	if (node == 0) return VFS_STATUS_INVALID;
	if (node->operations->open == 0) return VFS_STATUS_OK;

	return node->operations->open(node->context, flags);
}

static void devfs_close(vnode_t vnode, uint32_t flags)
{
	(void)flags;

	devfs_node_t *node = devfs_node(vnode);

	if (node != 0 && node->operations->close != 0) node->operations->close(node->context);
}

static vfs_status_t devfs_ioctl(vnode_t vnode, uint32_t command, void *argument)
{
	devfs_node_t *node = devfs_node(vnode);

	if (node == 0) return VFS_STATUS_INVALID;
	if (node->operations->ioctl == 0) return VFS_STATUS_NOT_SUPPORTED;

	return node->operations->ioctl(node->context, command, argument);
}

bool devfs_register(void)
{
	return vfs_register_filesystem(&g_devfs_filesystem);
}
