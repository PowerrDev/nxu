#include <vfs/devfs.h>

#include <kern/console/console.h>
#include <kern/ioready.h>
#include <kern/lock.h>
#include <kern/syscall/syscall_defs.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * One slot of the device table.
 *
 * A slot is in one of three states:
 *
 *   free        used is false.
 *   named       used and registered: the device has a name in /dev.
 *   detached    used but not registered: the device has no name, and lives
 *               only because files are still open on it. A device that was
 *               unregistered while open is detached, and so is every device
 *               made by a cloning open.
 *
 * The vnode is embedded, so a slot cannot be reused while anyone holds a
 * reference to its vnode. vnode_rele has no last-reference callback (vnode
 * reclamation is the filesystem's policy), so a detached slot is not freed the
 * moment its last file closes: it is reclaimed the next time a slot is wanted,
 * when its vnode is found back at its residency reference alone. Nothing can
 * gain a reference to a detached device in the meantime, because it has no
 * name to look up.
 */
typedef struct {
	struct vnode vnode;
	char name[VFS_NAME_MAX + 1U];
	const devfs_operations_t *operations;
	void *context;
	uint32_t uid;
	uint32_t gid;
	uint32_t mode;
	bool used;
	bool registered;
} devfs_node_t;

/*
 * g_devfs_lock protects the table: every slot's state, name and attributes,
 * and the making of a slot's vnode. It is a leaf and is never held across a
 * driver operation: operations and context do not change while a slot is in
 * use, and a caller that reached a slot through its vnode holds a reference
 * that keeps the slot from being reclaimed.
 */
static nxu_spinlock_t g_devfs_lock = NXU_SPINLOCK_INIT;
static devfs_node_t g_devfs_nodes[DEVFS_DEVICE_MAX];

/* Only one devfs is ever mounted; its root vnode lives here. */
static struct vnode g_devfs_root;
static mount_t g_devfs_mount;

static vfs_status_t devfs_mount(filesystem_t filesystem, block_device_t device, mount_t mount);
static vfs_status_t devfs_sync(mount_t mount);
static vfs_status_t devfs_unmount(mount_t mount);
static vfs_status_t devfs_lookup(vnode_t directory, const char *name, vnode_t *result);
static vfs_status_t devfs_readdir(vnode_t directory, uint64_t *offset, vfs_dirent_t *entry);
static vfs_status_t devfs_read(vnode_t vnode, uint64_t offset, void *buffer, uint64_t size, uint32_t ioflags,
							   uint64_t *read_size);
static vfs_status_t devfs_write(vnode_t vnode, uint64_t offset, const void *buffer, uint64_t size, uint32_t ioflags,
								uint64_t *written_size);
static vfs_status_t devfs_getattr(vnode_t vnode, vnode_attr_t *attr);
static vfs_status_t devfs_setattr(vnode_t vnode, const vnode_attr_t *attr, uint32_t which);
static vfs_status_t devfs_open(vnode_t vnode, uint32_t flags, vnode_t *opened);
static void devfs_close(vnode_t vnode, uint32_t flags);
static vfs_status_t devfs_ioctl(vnode_t vnode, uint32_t command, void *argument);
static vfs_status_t devfs_poll(vnode_t vnode, uint32_t *ready, struct ioready **source);

static const filesystem_operations_t g_devfs_filesystem_ops = {
	.mount = devfs_mount, .sync = devfs_sync, .unmount = devfs_unmount};

static const vnode_operations_t g_devfs_directory_ops = {
	.lookup = devfs_lookup, .readdir = devfs_readdir, .getattr = devfs_getattr};

static const vnode_operations_t g_devfs_device_ops = {.read = devfs_read,
													  .write = devfs_write,
													  .getattr = devfs_getattr,
													  .setattr = devfs_setattr,
													  .open = devfs_open,
													  .close = devfs_close,
													  .ioctl = devfs_ioctl,
													  .poll = devfs_poll};

static struct vfs_filesystem g_devfs_filesystem = {
	.fs_name = "devfs", .fs_ops = &g_devfs_filesystem_ops, .fs_registered = false};

static uint64_t
devfs_node_id(const devfs_node_t *node)
{
	return (uint64_t)(node - g_devfs_nodes) + 2ULL;
}

static devfs_node_t *
devfs_find_locked(const char *name)
{
	for (uint32_t index = 0U; index < DEVFS_DEVICE_MAX; index++) {
		devfs_node_t *node = &g_devfs_nodes[index];

		if (node->used && node->registered && strcmp(node->name, name) == 0) return node;
	}

	return 0;
}

/*
 * devfs_allocate_locked:
 *
 * A zeroed slot marked used, or 0 when the table is full. Detached slots
 * nobody refers to any more are reclaimed on the way.
 */
static devfs_node_t *
devfs_allocate_locked(void)
{
	devfs_node_t *found = 0;

	for (uint32_t index = 0U; index < DEVFS_DEVICE_MAX; index++) {
		devfs_node_t *node = &g_devfs_nodes[index];

		if (node->used && !node->registered &&
			(!node->vnode.v_active || __atomic_load_n(&node->vnode.v_refcount, __ATOMIC_ACQUIRE) <= 1U)) {
			node->vnode.v_active = false;
			node->used = false;
		}

		if (!node->used && found == 0) found = node;
	}

	if (found == 0) return 0;

	memset(found, 0, sizeof(*found));
	found->used = true;
	return found;
}

/* Make the vnode of a slot if this mount has not made it yet. Needs a mount. */
static void
devfs_activate_locked(devfs_node_t *node)
{
	if (node->vnode.v_active) return;

	vnode_init(&node->vnode, g_devfs_mount, &g_devfs_root, &g_devfs_device_ops, VNODE_TYPE_CHARACTER,
			   devfs_node_id(node), node);
}

static bool g_devfs_announce = true;

void
devfs_announce_registrations(bool announce)
{
	g_devfs_announce = announce;
}

bool
devfs_register_device(const char *name, const devfs_operations_t *operations, void *context)
{
	if (name == 0 || operations == 0 || name[0] == '\0') return false;

	size_t length = strlen(name);

	if (length > VFS_NAME_MAX) return false;

	for (size_t index = 0U; index < length; index++) {
		if (name[index] == '/') return false;
	}

	nxu_spin_lock(&g_devfs_lock);

	devfs_node_t *node = devfs_find_locked(name) == 0 ? devfs_allocate_locked() : 0;

	if (node != 0) {
		memcpy(node->name, name, length + 1U);
		node->operations = operations;
		node->context = context;
		node->mode = DEVFS_MODE_DEFAULT;
		node->registered = true;
	}

	nxu_spin_unlock(&g_devfs_lock);

	if (node != 0 && g_devfs_announce) kfslog("devfs", name, "registered at /dev/%s\n", name);

	return node != 0;
}

bool
devfs_unregister_device(const char *name)
{
	if (name == 0) return false;

	nxu_spin_lock(&g_devfs_lock);

	devfs_node_t *node = devfs_find_locked(name);

	/* Detached from here on; the slot is reclaimed once no file is open on it. */
	if (node != 0) node->registered = false;

	nxu_spin_unlock(&g_devfs_lock);
	return node != 0;
}

bool
devfs_set_attributes(const char *name, uint32_t uid, uint32_t gid, uint32_t mode)
{
	if (name == 0) return false;

	nxu_spin_lock(&g_devfs_lock);

	devfs_node_t *node = devfs_find_locked(name);

	if (node != 0) {
		node->uid = uid;
		node->gid = gid;
		node->mode = mode & 0777U;
	}

	nxu_spin_unlock(&g_devfs_lock);
	return node != 0;
}

uint32_t
devfs_device_count(void)
{
	uint32_t count = 0U;

	nxu_spin_lock(&g_devfs_lock);

	for (uint32_t index = 0U; index < DEVFS_DEVICE_MAX; index++) {
		if (g_devfs_nodes[index].used && g_devfs_nodes[index].registered) count++;
	}

	nxu_spin_unlock(&g_devfs_lock);
	return count;
}

const char *
devfs_device_name(uint32_t index)
{
	const char *name = 0;

	nxu_spin_lock(&g_devfs_lock);

	for (uint32_t slot = 0U; slot < DEVFS_DEVICE_MAX && name == 0; slot++) {
		if (!g_devfs_nodes[slot].used || !g_devfs_nodes[slot].registered) continue;

		if (index == 0U) name = g_devfs_nodes[slot].name;

		index--;
	}

	nxu_spin_unlock(&g_devfs_lock);
	return name;
}

static devfs_node_t *
devfs_node(vnode_t vnode)
{
	return vnode != 0 && vnode->v_type == VNODE_TYPE_CHARACTER ? vnode->v_data : 0;
}

static vfs_status_t
devfs_mount(filesystem_t filesystem, block_device_t device, mount_t mount)
{
	(void)filesystem;

	if (mount == 0 || device != 0 || g_devfs_mount != 0) return VFS_STATUS_INVALID;

	vnode_init(&g_devfs_root, mount, 0, &g_devfs_directory_ops, VNODE_TYPE_DIRECTORY, 1ULL, 0);
	g_devfs_mount = mount;
	mount->m_root = &g_devfs_root;
	mount->m_data = 0;
	kfslog("devfs", "devfs", "mounted at %s, %u device node slot(s)\n", mount->m_path, (uint32_t)DEVFS_DEVICE_MAX);
	return VFS_STATUS_OK;
}

static vfs_status_t
devfs_sync(mount_t mount)
{
	(void)mount;
	return VFS_STATUS_OK;
}

static vfs_status_t
devfs_unmount(mount_t mount)
{
	if (mount != g_devfs_mount) return VFS_STATUS_INVALID;

	if (g_devfs_root.v_refcount != 1U) return VFS_STATUS_BUSY;

	nxu_spin_lock(&g_devfs_lock);

	for (uint32_t index = 0U; index < DEVFS_DEVICE_MAX; index++) {
		const devfs_node_t *node = &g_devfs_nodes[index];

		if (node->used && node->vnode.v_active && node->vnode.v_refcount != 1U) {
			nxu_spin_unlock(&g_devfs_lock);
			return VFS_STATUS_BUSY;
		}
	}

	for (uint32_t index = 0U; index < DEVFS_DEVICE_MAX; index++)
		g_devfs_nodes[index].vnode.v_active = false;

	nxu_spin_unlock(&g_devfs_lock);

	g_devfs_root.v_active = false;
	g_devfs_mount = 0;
	mount->m_root = 0;
	return VFS_STATUS_OK;
}

static vfs_status_t
devfs_lookup(vnode_t directory, const char *name, vnode_t *result)
{
	*result = 0;

	if (directory != &g_devfs_root) return VFS_STATUS_INVALID;

	vfs_status_t status = VFS_STATUS_NOT_FOUND;

	nxu_spin_lock(&g_devfs_lock);

	devfs_node_t *node = devfs_find_locked(name);

	if (node != 0) {
		/* The vnode is made the first time the name is looked up after the mount; it then stays resident. */
		devfs_activate_locked(node);

		if (vnode_reference(&node->vnode)) {
			*result = &node->vnode;
			status = VFS_STATUS_OK;
		} else {
			status = VFS_STATUS_IO_ERROR;
		}
	}

	nxu_spin_unlock(&g_devfs_lock);
	return status;
}

/* The directory cookie is the next slot to look at, so entries keep their place as devices come and go. */
static vfs_status_t
devfs_readdir(vnode_t directory, uint64_t *offset, vfs_dirent_t *entry)
{
	if (directory != &g_devfs_root || offset == 0 || entry == 0) return VFS_STATUS_INVALID;

	vfs_status_t status = VFS_STATUS_END_OF_DIRECTORY;

	nxu_spin_lock(&g_devfs_lock);

	for (uint64_t index = *offset; index < DEVFS_DEVICE_MAX; index++) {
		const devfs_node_t *node = &g_devfs_nodes[index];

		if (!node->used || !node->registered) continue;

		entry->inode = devfs_node_id(node);
		entry->type = VNODE_TYPE_CHARACTER;
		entry->name_length = (uint32_t)strlen(node->name);
		memcpy(entry->name, node->name, (size_t)entry->name_length + 1U);
		*offset = index + 1ULL;
		status = VFS_STATUS_OK;
		break;
	}

	nxu_spin_unlock(&g_devfs_lock);
	return status;
}

static vfs_status_t
devfs_read(vnode_t vnode, uint64_t offset, void *buffer, uint64_t size, uint32_t ioflags, uint64_t *read_size)
{
	(void)offset;

	devfs_node_t *node = devfs_node(vnode);

	if (node == 0) return VFS_STATUS_INVALID;
	if (node->operations->read == 0) return VFS_STATUS_NOT_SUPPORTED;

	return node->operations->read(node->context, buffer, size, ioflags, read_size);
}

static vfs_status_t
devfs_write(vnode_t vnode, uint64_t offset, const void *buffer, uint64_t size, uint32_t ioflags,
			uint64_t *written_size)
{
	(void)offset;

	devfs_node_t *node = devfs_node(vnode);

	if (node == 0) return VFS_STATUS_INVALID;
	if (node->operations->write == 0) return VFS_STATUS_NOT_SUPPORTED;

	return node->operations->write(node->context, buffer, size, ioflags, written_size);
}

static vfs_status_t
devfs_getattr(vnode_t vnode, vnode_attr_t *attr)
{
	memset(attr, 0, sizeof(*attr));
	attr->va_inode = vnode->v_id;
	attr->va_type = vnode->v_type;
	attr->va_nlink = 1U;

	if (vnode == &g_devfs_root) {
		attr->va_mode = NXU_S_IFDIR | 0755U;
		return VFS_STATUS_OK;
	}

	devfs_node_t *node = devfs_node(vnode);

	if (node == 0) return VFS_STATUS_INVALID;

	nxu_spin_lock(&g_devfs_lock);

	attr->va_mode = NXU_S_IFCHR | node->mode;
	attr->va_uid = node->uid;
	attr->va_gid = node->gid;
	attr->va_rdev = (uint64_t)(node - g_devfs_nodes);

	nxu_spin_unlock(&g_devfs_lock);
	return VFS_STATUS_OK;
}

static vfs_status_t
devfs_setattr(vnode_t vnode, const vnode_attr_t *attr, uint32_t which)
{
	devfs_node_t *node = devfs_node(vnode);

	if (node == 0) return VFS_STATUS_INVALID;

	nxu_spin_lock(&g_devfs_lock);

	if ((which & VNODE_ATTR_MODE) != 0U) node->mode = attr->va_mode & 0777U;
	if ((which & VNODE_ATTR_UID) != 0U) node->uid = attr->va_uid;
	if ((which & VNODE_ATTR_GID) != 0U) node->gid = attr->va_gid;

	nxu_spin_unlock(&g_devfs_lock);
	return VFS_STATUS_OK;
}

/*
 * devfs_open:
 *
 * Ask the device whether it may be opened. A cloning device answers with the
 * operations and context of a new device instead; that device gets a detached
 * slot of its own, which starts with the attributes of the device that made
 * it, and the open is redirected to it through *opened. If no slot can be
 * had, the new device is closed again so its driver can release it.
 */
static vfs_status_t
devfs_open(vnode_t vnode, uint32_t flags, vnode_t *opened)
{
	devfs_node_t *node = devfs_node(vnode);

	if (node == 0) return VFS_STATUS_INVALID;
	if (node->operations->open == 0) return VFS_STATUS_OK;

	devfs_clone_t clone = {0};
	vfs_status_t status = node->operations->open(node->context, flags, &clone);

	if (status != VFS_STATUS_OK || clone.operations == 0) return status;

	nxu_spin_lock(&g_devfs_lock);

	devfs_node_t *made = g_devfs_mount != 0 ? devfs_allocate_locked() : 0;

	if (made != 0) {
		made->operations = clone.operations;
		made->context = clone.context;
		made->uid = node->uid;
		made->gid = node->gid;
		made->mode = node->mode;

		devfs_activate_locked(made);

		/* Cannot fail: the vnode was made a moment ago under this lock. */
		(void)vnode_reference(&made->vnode);
	}

	nxu_spin_unlock(&g_devfs_lock);

	if (made == 0) {
		if (clone.operations->close != 0) clone.operations->close(clone.context);

		return VFS_STATUS_NO_SPACE;
	}

	*opened = &made->vnode;
	return VFS_STATUS_OK;
}

static void
devfs_close(vnode_t vnode, uint32_t flags)
{
	(void)flags;

	devfs_node_t *node = devfs_node(vnode);

	if (node != 0 && node->operations->close != 0) node->operations->close(node->context);
}

static vfs_status_t
devfs_ioctl(vnode_t vnode, uint32_t command, void *argument)
{
	devfs_node_t *node = devfs_node(vnode);

	if (node == 0) return VFS_STATUS_INVALID;
	if (node->operations->ioctl == 0) return VFS_STATUS_NOT_TTY;

	return node->operations->ioctl(node->context, command, argument);
}

static vfs_status_t
devfs_poll(vnode_t vnode, uint32_t *ready, struct ioready **source)
{
	devfs_node_t *node = devfs_node(vnode);

	if (node == 0) return VFS_STATUS_INVALID;

	if (node->operations->poll == 0) {
		*ready = IOREADY_READ | IOREADY_WRITE;
		return VFS_STATUS_OK;
	}

	return node->operations->poll(node->context, ready, source);
}

bool
devfs_register(void)
{
	return vfs_register_filesystem(&g_devfs_filesystem);
}
