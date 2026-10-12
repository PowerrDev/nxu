#include <vfs/vfs_internal.h>
#include <kern/memory/heap.h>
#include <libk/string.h>

#include <stdint.h>

/*
 * vfs_lookup:
 *
 * Resolve an absolute pathname from the root vnode of the longest matching
 * mount. Every intermediate vnode reference is released before the next
 * component is entered.
 */
vfs_status_t
vfs_lookup(const char *path, vnode_t *result)
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
		while (*cursor == '/')
			cursor++;
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
vfs_status_t
vfs_lookup_parent(const char *path, vnode_t *parent, char name[VFS_NAME_MAX + 1U])
{
	if (parent != 0) *parent = 0;
	if (path == 0 || parent == 0 || name == 0 || path[0] != '/') return VFS_STATUS_INVALID;

	uint32_t length = 0U;
	while (length < VFS_PATH_MAX && path[length] != '\0')
		length++;
	if (length == VFS_PATH_MAX) return VFS_STATUS_PATH_TOO_LONG;

	if (length <= 1U || path[length - 1U] == '/') return VFS_STATUS_INVALID;

	uint32_t slash = length;
	while (slash != 0U && path[slash - 1U] != '/')
		slash--;

	uint32_t name_length = length - slash;
	if (name_length == 0U) return VFS_STATUS_INVALID;

	if (name_length > VFS_NAME_MAX) return VFS_STATUS_NAME_TOO_LONG;

	for (uint32_t index = 0U; index < name_length; index++)
		name[index] = path[slash + index];
	name[name_length] = '\0';

	if ((name_length == 1U && name[0] == '.') || (name_length == 2U && name[0] == '.' && name[1] == '.')) {
		return VFS_STATUS_INVALID;
	}

	char parent_path[VFS_PATH_MAX];
	uint32_t parent_length = slash == 0U ? 1U : slash;
	if (parent_length > 1U && path[parent_length - 1U] == '/') parent_length--;

	if (parent_length == 0U) parent_length = 1U;
	for (uint32_t index = 0U; index < parent_length; index++)
		parent_path[index] = path[index];
	if (parent_length == 1U) parent_path[0] = '/';
	parent_path[parent_length] = '\0';

	return vfs_lookup(parent_path, parent);
}

vfs_status_t
vfs_create(const char *path, vnode_type_t type, vnode_t *result)
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

vfs_status_t
vfs_mkdir(const char *path)
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
vfs_status_t
vfs_unlink_held(const char *path, vnode_t *expected)
{
	if (path == 0) return VFS_STATUS_INVALID;

	vnode_t target;
	vfs_status_t status = vfs_lookup(path, &target);
	if (status != VFS_STATUS_OK) return status;

	if (expected != 0 && *expected != 0 &&
		(target->v_id != (*expected)->v_id || target->v_mount != (*expected)->v_mount)) {
		vnode_rele(target);
		return VFS_STATUS_BUSY;
	}
	if (target->v_type == VNODE_TYPE_DIRECTORY) {
		vnode_rele(target);
		return VFS_STATUS_IS_DIRECTORY;
	}
	vnode_rele(target);

	vnode_t parent;
	char name[VFS_NAME_MAX + 1U];
	status = vfs_lookup_parent(path, &parent, name);
	if (status != VFS_STATUS_OK) return status;

	if (expected != 0 && *expected != 0) {
		status = parent->v_ops != 0 && parent->v_ops->unlink_held != 0
					 ? parent->v_ops->unlink_held(parent, name, expected)
					 : VFS_STATUS_NOT_SUPPORTED;
	} else
		status = vnode_unlink(parent, name);
	vnode_rele(parent);
	return status;
}

vfs_status_t
vfs_unlink(const char *path)
{
	return vfs_unlink_held(path, 0);
}

/* Empty directory removal is separate from regular-file unlink. */
vfs_status_t
vfs_rmdir(const char *path)
{
	vnode_t target;
	vfs_status_t status = vfs_lookup(path, &target);
	if (status != VFS_STATUS_OK) return status;
	if (target->v_type != VNODE_TYPE_DIRECTORY)
		status = VFS_STATUS_NOT_DIRECTORY;
	else if (target->v_mount == 0 || target == target->v_mount->m_root)
		status = VFS_STATUS_BUSY;
	vnode_rele(target);
	if (status != VFS_STATUS_OK) return status;
	vnode_t parent;
	char name[VFS_NAME_MAX + 1U];
	status = vfs_lookup_parent(path, &parent, name);
	if (status != VFS_STATUS_OK) return status;
	status =
		parent->v_ops != 0 && parent->v_ops->rmdir != 0 ? parent->v_ops->rmdir(parent, name) : VFS_STATUS_NOT_SUPPORTED;
	vnode_rele(parent);
	return status;
}

/* SAP holds every directory on the current branch. No pathname replacement
 * can substitute a different directory while approval or traversal is active. */
typedef struct {
	vnode_t directory;
	char name[VFS_NAME_MAX + 1U];
	uint32_t parent_length;
} vfs_tree_frame_t;

static bool
vfs_tree_contains_mount(const char *path)
{
	size_t length = strlen(path);
	vfs_mount_info_t mount;
	for (uint32_t index = 0; vfs_mount_get(index, &mount); index++)
		if (strlen(mount.path) >= length && memcmp(mount.path, path, length) == 0 &&
			(mount.path[length] == '/' || mount.path[length] == 0))
			return true;
	return false;
}

vfs_status_t
vfs_remove_tree_held(const char *path, vnode_t *expected, bool (*cancelled)(void))
{
	if (path == 0 || expected == 0 || *expected == 0) return VFS_STATUS_INVALID;
	if (strlen(path) >= VFS_PATH_MAX) return VFS_STATUS_PATH_TOO_LONG;
	vnode_t root = *expected;
	if (root->v_type != VNODE_TYPE_DIRECTORY) return VFS_STATUS_NOT_DIRECTORY;
	if (root->v_mount == 0 || root == root->v_mount->m_root || vfs_tree_contains_mount(path)) return VFS_STATUS_BUSY;
	vnode_t current;
	vfs_status_t status = vfs_lookup(path, &current);
	if (status != VFS_STATUS_OK) return status;
	bool matches = current == root;
	vnode_rele(current);
	if (!matches) return VFS_STATUS_BUSY;

	vnode_t parent;
	char name[VFS_NAME_MAX + 1U];
	status = vfs_lookup_parent(path, &parent, name);
	if (status != VFS_STATUS_OK) return status;
	/* Allocate the bounded traversal stack off the kernel thread's stack. */
	const uint32_t maximum = VFS_PATH_MAX / 2U;
	vfs_tree_frame_t *frames = kmalloc(sizeof(*frames) * maximum);
	if (frames == 0) {
		vnode_rele(parent);
		return VFS_STATUS_NO_MEMORY;
	}
	uint32_t depth = 1U;
	frames[0] = (vfs_tree_frame_t){.directory = root};
	memcpy(frames[0].name, name, strlen(name) + 1U);
	*expected = 0;
	char current_path[VFS_PATH_MAX];
	memcpy(current_path, path, strlen(path) + 1U);
	while (depth != 0U) {
		if (cancelled != 0 && cancelled()) {
			status = VFS_STATUS_INTERRUPTED;
			break;
		}
		if (vfs_tree_contains_mount(path)) {
			status = VFS_STATUS_BUSY;
			break;
		}
		vfs_tree_frame_t *frame = &frames[depth - 1U];
		vnode_t directory = frame->directory;
		uint64_t offset = 0;
		vfs_dirent_t entry;
		do {
			status = vnode_readdir(directory, &offset, &entry);
		} while (status == VFS_STATUS_OK && (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0));
		if (status == VFS_STATUS_END_OF_DIRECTORY) {
			vnode_t owner = depth == 1U ? parent : frames[depth - 2U].directory;
			status = owner->v_ops != 0 && owner->v_ops->rmdir_held != 0
						 ? owner->v_ops->rmdir_held(owner, frame->name, &frame->directory)
						 : VFS_STATUS_NOT_SUPPORTED;
			if (status != VFS_STATUS_OK) break;
			current_path[frame->parent_length] = 0;
			depth--;
			continue;
		}
		if (status != VFS_STATUS_OK) break;
		if (entry.name_length == 0U || entry.name_length > VFS_NAME_MAX || entry.name[entry.name_length] != 0) {
			status = VFS_STATUS_NAME_TOO_LONG;
			break;
		}
		bool slash = false;
		for (uint32_t index = 0; index < entry.name_length; index++)
			if (entry.name[index] == '/') slash = true;
		if (slash) {
			status = VFS_STATUS_INVALID;
			break;
		}
		uint32_t length = (uint32_t)strlen(current_path);
		if (length + 1U + entry.name_length >= VFS_PATH_MAX || depth == maximum) {
			status = VFS_STATUS_PATH_TOO_LONG;
			break;
		}
		vnode_t child;
		status = vnode_lookup(directory, entry.name, &child);
		if (status != VFS_STATUS_OK) break;
		if (child->v_mount != root->v_mount || child == child->v_mount->m_root) {
			vnode_rele(child);
			status = VFS_STATUS_BUSY;
			break;
		}
		if (child->v_type == VNODE_TYPE_DIRECTORY) {
			frames[depth] = (vfs_tree_frame_t){.directory = child, .parent_length = length};
			memcpy(frames[depth].name, entry.name, entry.name_length + 1U);
			current_path[length] = '/';
			memcpy(current_path + length + 1U, entry.name, entry.name_length + 1U);
			depth++;
		} else {
			status = child->v_type == VNODE_TYPE_REGULAR && directory->v_ops != 0 && directory->v_ops->unlink_held != 0
						 ? directory->v_ops->unlink_held(directory, entry.name, &child)
						 : VFS_STATUS_NOT_SUPPORTED;
			if (child != 0) vnode_rele(child);
			if (status != VFS_STATUS_OK) break;
		}
	}
	for (uint32_t index = 0; index < depth; index++)
		if (frames[index].directory != 0) vnode_rele(frames[index].directory);
	vnode_rele(parent);
	(void)kfree(frames);
	return status;
}
