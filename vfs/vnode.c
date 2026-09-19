#include <vfs/vnode.h>

#include <stdbool.h>
#include <stdint.h>

void vnode_init(
	vnode_t vnode,
	mount_t mount,
	vnode_t parent,
	const vnode_operations_t *operations,
	vnode_type_t type,
	uint64_t identifier,
	void *data
)
{
	if (vnode == 0) return;

	vnode->v_mount = mount;
	vnode->v_parent = parent;
	vnode->v_ops = operations;
	vnode->v_data = data;
	vnode->v_id = identifier;
	vnode->v_size = 0ULL;
	vnode->v_refcount = 1U;
	vnode->v_type = type;
	vnode->v_active = true;
}

bool vnode_reference(vnode_t vnode)
{
	if (vnode == 0 || !vnode->v_active) return false;

	uint32_t old = __atomic_load_n(&vnode->v_refcount, __ATOMIC_ACQUIRE);

	for (;;) {
		if (old == UINT32_MAX) return false;

		if (__atomic_compare_exchange_n(
			&vnode->v_refcount,
			&old,
			old + 1U,
			false,
			__ATOMIC_ACQ_REL,
			__ATOMIC_ACQUIRE
		)) {
			return true;
		}
	}
}

void vnode_rele(vnode_t vnode)
{
	if (vnode == 0) return;

	uint32_t old = __atomic_load_n(&vnode->v_refcount, __ATOMIC_ACQUIRE);

	while (old != 0U) {
		if (__atomic_compare_exchange_n(
			&vnode->v_refcount,
			&old,
			old - 1U,
			false,
			__ATOMIC_ACQ_REL,
			__ATOMIC_ACQUIRE
		)) {
			return;
		}
	}
}

vfs_status_t vnode_lookup(vnode_t directory, const char *name, vnode_t *result)
{
	if (result != 0) *result = 0;

	if (directory == 0 || name == 0 || result == 0) return VFS_STATUS_INVALID;

	if (!directory->v_active) return VFS_STATUS_INVALID;
	if (directory->v_type != VNODE_TYPE_DIRECTORY) return VFS_STATUS_NOT_DIRECTORY;

	if (directory->v_ops == 0 || directory->v_ops->lookup == 0) return VFS_STATUS_NOT_SUPPORTED;

	return directory->v_ops->lookup(directory, name, result);
}

vfs_status_t vnode_create(vnode_t directory, const char *name, vnode_type_t type, vnode_t *result)
{
	if (result != 0) *result = 0;

	if (directory == 0 || name == 0 || result == 0) return VFS_STATUS_INVALID;

	if (!directory->v_active) return VFS_STATUS_INVALID;
	if (directory->v_type != VNODE_TYPE_DIRECTORY) return VFS_STATUS_NOT_DIRECTORY;

	if (directory->v_ops == 0 || directory->v_ops->create == 0) return VFS_STATUS_NOT_SUPPORTED;

	return directory->v_ops->create(directory, name, type, result);
}

/*
 * vnode_unlink:
 *
 * Remove one non-directory name from a directory. Namespace mutation is
 * delegated to the filesystem so on-disk link counts and storage ownership
 * remain filesystem responsibilities.
 */
vfs_status_t vnode_unlink(vnode_t directory, const char *name)
{
	if (directory == 0 || name == 0) return VFS_STATUS_INVALID;

	if (!directory->v_active) return VFS_STATUS_INVALID;
	if (directory->v_type != VNODE_TYPE_DIRECTORY) return VFS_STATUS_NOT_DIRECTORY;

	if (directory->v_ops == 0 || directory->v_ops->unlink == 0) return VFS_STATUS_NOT_SUPPORTED;

	return directory->v_ops->unlink(directory, name);
}

/*
 * Routine:     vnode_readdir
 * Purpose:
 *              Enumerate one directory entry using a filesystem-owned
 *              byte/index cookie. The filesystem advances the cookie only
 *              after consuming a valid directory record.
 */
vfs_status_t
vnode_readdir(vnode_t directory, uint64_t *offset, vfs_dirent_t *entry)
{
	if (directory == 0 || offset == 0 || entry == 0) return VFS_STATUS_INVALID;

	if (!directory->v_active) return VFS_STATUS_INVALID;
	if (directory->v_type != VNODE_TYPE_DIRECTORY) return VFS_STATUS_NOT_DIRECTORY;

	if (directory->v_ops == 0 || directory->v_ops->readdir == 0) return VFS_STATUS_NOT_SUPPORTED;

	return directory->v_ops->readdir(directory, offset, entry);
}

vfs_status_t vnode_read(
	vnode_t vnode,
	uint64_t offset,
	void *buffer,
	uint64_t size,
	uint64_t *read_size
)
{
	if (read_size != 0) *read_size = 0ULL;

	if (vnode == 0 || buffer == 0 || read_size == 0) return VFS_STATUS_INVALID;

	if (!vnode->v_active) return VFS_STATUS_INVALID;
	if (vnode->v_type == VNODE_TYPE_DIRECTORY) return VFS_STATUS_IS_DIRECTORY;

	if (vnode->v_ops == 0 || vnode->v_ops->read == 0) return VFS_STATUS_NOT_SUPPORTED;

	return vnode->v_ops->read(vnode, offset, buffer, size, read_size);
}

vfs_status_t vnode_write(
	vnode_t vnode,
	uint64_t offset,
	const void *buffer,
	uint64_t size,
	uint64_t *written_size
)
{
	if (written_size != 0) *written_size = 0ULL;

	if (vnode == 0 || buffer == 0 || written_size == 0) return VFS_STATUS_INVALID;

	if (!vnode->v_active) return VFS_STATUS_INVALID;
	if (vnode->v_type == VNODE_TYPE_DIRECTORY) return VFS_STATUS_IS_DIRECTORY;

	if (vnode->v_ops == 0 || vnode->v_ops->write == 0) return VFS_STATUS_NOT_SUPPORTED;

	return vnode->v_ops->write(vnode, offset, buffer, size, written_size);
}

vfs_status_t vnode_truncate(vnode_t vnode, uint64_t size)
{
	if (vnode == 0 || !vnode->v_active) return VFS_STATUS_INVALID;

	if (vnode->v_type == VNODE_TYPE_DIRECTORY) return VFS_STATUS_IS_DIRECTORY;
	if (vnode->v_ops == 0 || vnode->v_ops->truncate == 0) return VFS_STATUS_NOT_SUPPORTED;

	return vnode->v_ops->truncate(vnode, size);
}

vfs_status_t vnode_getattr(vnode_t vnode, vnode_attr_t *attr)
{
	if (vnode == 0 || attr == 0 || !vnode->v_active) return VFS_STATUS_INVALID;

	if (vnode->v_ops == 0 || vnode->v_ops->getattr == 0) return VFS_STATUS_NOT_SUPPORTED;

	return vnode->v_ops->getattr(vnode, attr);
}

/*
 * vnode_readlink:
 *
 * Copy the target of a symbolic link into buffer (not NUL terminated).
 * *length receives the target length; a target longer than capacity fails
 * with NAME_TOO_LONG rather than being truncated.
 */
vfs_status_t vnode_readlink(vnode_t vnode, char *buffer, uint64_t capacity, uint64_t *length)
{
	if (length != 0) *length = 0ULL;

	if (vnode == 0 || buffer == 0 || length == 0 || !vnode->v_active) return VFS_STATUS_INVALID;
	if (vnode->v_type != VNODE_TYPE_SYMLINK) return VFS_STATUS_INVALID;

	if (vnode->v_ops == 0 || vnode->v_ops->readlink == 0) return VFS_STATUS_NOT_SUPPORTED;

	return vnode->v_ops->readlink(vnode, buffer, capacity, length);
}
