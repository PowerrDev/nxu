#include <vfs/vfs_internal.h>

#include <stdint.h>

/*
 * vfs_lookup:
 *
 * Resolve an absolute pathname from the root vnode of the longest matching
 * mount. Every intermediate vnode reference is released before the next
 * component is entered.
 */
vfs_status_t vfs_lookup(const char *path, vnode_t *result)
{
	if (result != 0) *result = 0;
	if (path == 0 || result == 0 || path[0] != '/') return VFS_STATUS_INVALID;

	mount_t mount;
	const char *cursor;
	vfs_status_t status = vfs_path_select_mount(path, &mount, &cursor);
	if (status != VFS_STATUS_OK) return status;

	if (mount->m_root == 0 || !vnode_reference(mount->m_root)) return VFS_STATUS_IO_ERROR;

	vnode_t current = mount->m_root;
	char component[VFS_NAME_MAX + 1U];

	while (*cursor != '\0') {
		while (*cursor == '/') cursor++;
		if (*cursor == '\0') break;

		uint32_t length = 0U;
		while (cursor[length] != '\0' && cursor[length] != '/') {
			if (length >= VFS_NAME_MAX) {
				vnode_rele(current);
				return VFS_STATUS_NAME_TOO_LONG;
			}
			component[length] = cursor[length];
			length++;
		}
		component[length] = '\0';
		cursor += length;

		if (length == 1U && component[0] == '.') continue;


		if (length == 2U && component[0] == '.' && component[1] == '.') {
			vnode_t parent = current->v_parent;
			if (parent != 0 && vnode_reference(parent)) {
				vnode_rele(current);
				current = parent;
			}
			continue;
		}

		vnode_t next;
		status = vnode_lookup(current, component, &next);
		vnode_rele(current);
		if (status != VFS_STATUS_OK) return status;
		current = next;
	}

	*result = current;
	return VFS_STATUS_OK;
}

/*
 * vfs_lookup_parent:
 *
 * Resolve every component except the final name. The final component is
 * returned separately for create operations.
 */
vfs_status_t vfs_lookup_parent(const char *path, vnode_t *parent, char name[VFS_NAME_MAX + 1U])
{
	if (parent != 0) *parent = 0;
	if (path == 0 || parent == 0 || name == 0 || path[0] != '/') return VFS_STATUS_INVALID;

	uint32_t length = 0U;
	while (length < VFS_PATH_MAX && path[length] != '\0') length++;
	if (length == VFS_PATH_MAX) return VFS_STATUS_PATH_TOO_LONG;

	if (length <= 1U || path[length - 1U] == '/') return VFS_STATUS_INVALID;

	uint32_t slash = length;
	while (slash != 0U && path[slash - 1U] != '/') slash--;

	uint32_t name_length = length - slash;
	if (name_length == 0U) return VFS_STATUS_INVALID;

	if (name_length > VFS_NAME_MAX) return VFS_STATUS_NAME_TOO_LONG;

	for (uint32_t index = 0U; index < name_length; index++) name[index] = path[slash + index];
	name[name_length] = '\0';

	if (
		(name_length == 1U && name[0] == '.') ||
		(name_length == 2U && name[0] == '.' && name[1] == '.')
	) {
		return VFS_STATUS_INVALID;
	}

	char parent_path[VFS_PATH_MAX];
	uint32_t parent_length = slash == 0U ? 1U : slash;
	if (parent_length > 1U && path[parent_length - 1U] == '/') parent_length--;

	if (parent_length == 0U) parent_length = 1U;
	for (uint32_t index = 0U; index < parent_length; index++) parent_path[index] = path[index];
	if (parent_length == 1U) parent_path[0] = '/';
	parent_path[parent_length] = '\0';

	return vfs_lookup(parent_path, parent);
}

vfs_status_t vfs_create(const char *path, vnode_type_t type, vnode_t *result)
{
	if (result != 0) *result = 0;
	if (path == 0 || result == 0) return VFS_STATUS_INVALID;

	if (type != VNODE_TYPE_REGULAR && type != VNODE_TYPE_DIRECTORY) return VFS_STATUS_NOT_SUPPORTED;

	vnode_t parent;
	char name[VFS_NAME_MAX + 1U];
	vfs_status_t status = vfs_lookup_parent(path, &parent, name);
	if (status != VFS_STATUS_OK) return status;

	vnode_t existing;
	status = vnode_lookup(parent, name, &existing);
	if (status == VFS_STATUS_OK) {
		vnode_rele(existing);
		vnode_rele(parent);
		return VFS_STATUS_EXISTS;
	}

	if (status != VFS_STATUS_NOT_FOUND) {
		vnode_rele(parent);
		return status;
	}

	status = vnode_create(parent, name, type, result);
	vnode_rele(parent);
	return status;
}

vfs_status_t vfs_mkdir(const char *path)
{
	vnode_t vnode;
	vfs_status_t status = vfs_create(path, VNODE_TYPE_DIRECTORY, &vnode);
	if (status == VFS_STATUS_OK) vnode_rele(vnode);
	return status;
}

/*
 * vfs_unlink:
 *
 * Resolve the parent directory and remove one regular-file name through the
 * filesystem vnode operation. Directory removal is intentionally separate
 * from unlink semantics.
 */
vfs_status_t vfs_unlink(const char *path)
{
	if (path == 0) return VFS_STATUS_INVALID;

	vnode_t target;
	vfs_status_t status = vfs_lookup(path, &target);
	if (status != VFS_STATUS_OK) return status;

	if (target->v_type == VNODE_TYPE_DIRECTORY) {
		vnode_rele(target);
		return VFS_STATUS_IS_DIRECTORY;
	}
	vnode_rele(target);

	vnode_t parent;
	char name[VFS_NAME_MAX + 1U];
	status = vfs_lookup_parent(path, &parent, name);
	if (status != VFS_STATUS_OK) return status;

	status = vnode_unlink(parent, name);
	vnode_rele(parent);
	return status;
}
