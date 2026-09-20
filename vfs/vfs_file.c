#include <vfs/vfs.h>

#include <stdint.h>

vfs_status_t vfs_open(filedesc_t filedesc, const char *path, uint32_t flags, uint32_t *descriptor)
{
	if (descriptor != 0) *descriptor = UINT32_MAX;
	if (filedesc == 0 || path == 0 || descriptor == 0) return VFS_STATUS_INVALID;

	if ((flags & (VFS_OPEN_READ | VFS_OPEN_WRITE)) == 0U) return VFS_STATUS_INVALID;
	if ((flags & VFS_OPEN_TRUNCATE) != 0U && (flags & VFS_OPEN_WRITE) == 0U) return VFS_STATUS_INVALID;

	vnode_t vnode;
	vfs_status_t status = vfs_lookup(path, &vnode);

	if (status == VFS_STATUS_NOT_FOUND && (flags & VFS_OPEN_CREATE) != 0U) {
		status = vfs_create(path, VNODE_TYPE_REGULAR, &vnode);
	}

	if (status != VFS_STATUS_OK) return status;

	if (vnode->v_type == VNODE_TYPE_DIRECTORY && flags != VFS_OPEN_READ) {
		vnode_rele(vnode);
		return VFS_STATUS_IS_DIRECTORY;
	}

	if ((flags & VFS_OPEN_TRUNCATE) != 0U) {
		status = vnode_truncate(vnode, 0ULL);
		if (status != VFS_STATUS_OK) {
			vnode_rele(vnode);
			return status;
		}
	}

	/* A device node may refuse the open (it is exclusive, or the caller lacks a privilege). */
	status = vnode_open(vnode, flags);
	if (status != VFS_STATUS_OK) {
		vnode_rele(vnode);
		return status;
	}

	file_t file;
	status = file_alloc(vnode, flags, &file);

	if (status != VFS_STATUS_OK) {
		vnode_close(vnode, flags);
		vnode_rele(vnode);
		return status;
	}

	vnode_rele(vnode);

	status = filedesc_install(filedesc, file, descriptor);
	if (status != VFS_STATUS_OK) file_rele(file);
	return status;
}

vfs_status_t vfs_close(filedesc_t filedesc, uint32_t descriptor)
{
	file_t file;
	vfs_status_t status = filedesc_remove(filedesc, descriptor, &file);
	if (status != VFS_STATUS_OK) return status;

	file_rele(file);
	return VFS_STATUS_OK;
}

vfs_status_t vfs_read(
	filedesc_t filedesc,
	uint32_t descriptor,
	void *buffer,
	uint64_t size,
	uint64_t *read_size
)
{
	file_t file;
	vfs_status_t status = filedesc_get(filedesc, descriptor, &file);
	if (status != VFS_STATUS_OK) return status;

	status = file_read(file, buffer, size, read_size);
	file_rele(file);
	return status;
}

/*
 * Routine:     vfs_readdir
 * Purpose:
 *              Resolve a process descriptor to its open directory and return
 *              the next filesystem-independent directory entry.
 */
vfs_status_t vfs_readdir(filedesc_t filedesc, uint32_t descriptor, vfs_dirent_t *entry)
{
	file_t file;
	vfs_status_t status = filedesc_get(filedesc, descriptor, &file);
	if (status != VFS_STATUS_OK) return status;

	status = file_readdir(file, entry);
	file_rele(file);
	return status;
}

vfs_status_t vfs_write(
	filedesc_t filedesc,
	uint32_t descriptor,
	const void *buffer,
	uint64_t size,
	uint64_t *written_size
)
{
	file_t file;
	vfs_status_t status = filedesc_get(filedesc, descriptor, &file);
	if (status != VFS_STATUS_OK) return status;

	status = file_write(file, buffer, size, written_size);
	file_rele(file);
	return status;
}

vfs_status_t vfs_seek(filedesc_t filedesc, uint32_t descriptor, uint64_t offset)
{
	file_t file;
	vfs_status_t status = filedesc_get(filedesc, descriptor, &file);
	if (status != VFS_STATUS_OK) return status;

	status = file_seek(file, offset);
	file_rele(file);
	return status;
}

/*
 * vfs_ioctl:
 *
 * A device-specific request on an open descriptor. argument is a kernel
 * buffer of the size the command encodes.
 */
vfs_status_t vfs_ioctl(filedesc_t filedesc, uint32_t descriptor, uint32_t command, void *argument)
{
	file_t file;
	vfs_status_t status = filedesc_get(filedesc, descriptor, &file);
	if (status != VFS_STATUS_OK) return status;

	status = file_ioctl(file, command, argument);
	file_rele(file);
	return status;
}
