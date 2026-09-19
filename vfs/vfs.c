#include <kern/console/console.h>
#include <mach/machine/cpu.h>
#include <vfs/vfs.h>
#include <vfs/vfs_internal.h>

#include <platform/uart.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	volatile uint32_t value;
} vfs_lock_t;

static filesystem_t g_filesystems[VFS_FILESYSTEM_MAX];
static struct mount g_mounts[VFS_MOUNT_MAX];
static uint32_t g_filesystem_count;
static uint32_t g_mount_count;
static vfs_lock_t g_vfs_lock;
static bool g_vfs_initialized;

static void vfs_lock(vfs_lock_t *lock)
{
	while (__atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE) != 0U) {
		cpu_relax();
	}
}

static void vfs_unlock(vfs_lock_t *lock)
{
	__atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

static uint32_t vfs_string_length(const char *string, uint32_t limit)
{
	uint32_t length = 0U;
	while (length < limit && string[length] != '\0') length++;
	return length;
}

static bool vfs_copy_string(char *destination, uint32_t capacity, const char *source)
{
	if (destination == 0 || source == 0 || capacity == 0U) return false;

	uint32_t index = 0U;
	while (index + 1U < capacity && source[index] != '\0') {
		destination[index] = source[index];
		index++;
	}

	if (source[index] != '\0') return false;
	destination[index] = '\0';
	return true;
}

static filesystem_t vfs_find_filesystem_locked(const char *name)
{
	for (uint32_t index = 0U; index < VFS_FILESYSTEM_MAX; index++) {
		filesystem_t filesystem = g_filesystems[index];
		if (filesystem == 0 || !filesystem->fs_registered) continue;

		uint32_t left = vfs_string_length(filesystem->fs_name, VFS_FILESYSTEM_NAME_MAX + 1U);
		uint32_t right = vfs_string_length(name, VFS_FILESYSTEM_NAME_MAX + 1U);
		if (left != right) continue;

		bool equal = true;
		for (uint32_t offset = 0U; offset < left; offset++) {
			if (filesystem->fs_name[offset] != name[offset]) {
				equal = false;
				break;
			}
		}

		if (equal) return filesystem;
	}

	return 0;
}

bool vfs_init(void)
{
	vfs_lock(&g_vfs_lock);

	if (g_vfs_initialized) {
		vfs_unlock(&g_vfs_lock);
		return true;
	}

	memset(g_filesystems, 0, sizeof(g_filesystems));
	memset(g_mounts, 0, sizeof(g_mounts));
	g_filesystem_count = 0U;
	g_mount_count = 0U;

	if (!file_table_init()) {
		vfs_unlock(&g_vfs_lock);
		return false;
	}

	g_vfs_initialized = true;
	vfs_unlock(&g_vfs_lock);
	return true;
}

/*
 * vfs_register_filesystem:
 *
 * Publish one filesystem type. Registration does not create a mount or take
 * ownership of filesystem storage.
 */
bool vfs_register_filesystem(filesystem_t filesystem)
{
	if (!g_vfs_initialized || filesystem == 0 || filesystem->fs_ops == 0) return false;

	if (filesystem->fs_name[0] == '\0') return false;

	vfs_lock(&g_vfs_lock);

	if (filesystem->fs_registered || vfs_find_filesystem_locked(filesystem->fs_name) != 0) {
		vfs_unlock(&g_vfs_lock);
		return false;
	}

	for (uint32_t index = 0U; index < VFS_FILESYSTEM_MAX; index++) {
		if (g_filesystems[index] != 0) continue;

		g_filesystems[index] = filesystem;
		filesystem->fs_registered = true;
		g_filesystem_count++;

		vfs_unlock(&g_vfs_lock);
		return true;
	}

	vfs_unlock(&g_vfs_lock);
	return false;
}

vfs_status_t vfs_mount(const char *filesystem_name, block_device_t device, const char *path)
{
	if (!g_vfs_initialized || filesystem_name == 0 || path == 0 || path[0] != '/') return VFS_STATUS_INVALID;

	uint32_t path_length = vfs_string_length(path, VFS_PATH_MAX);
	if (path_length == VFS_PATH_MAX) return VFS_STATUS_PATH_TOO_LONG;

	if (path_length > 1U && path[path_length - 1U] == '/') return VFS_STATUS_INVALID;

	filesystem_t filesystem;
	struct mount *mount = 0;

	vfs_lock(&g_vfs_lock);
	filesystem = vfs_find_filesystem_locked(filesystem_name);

	if (filesystem == 0 || filesystem->fs_ops->mount == 0) {
		vfs_unlock(&g_vfs_lock);
		return VFS_STATUS_NOT_FOUND;
	}

	for (uint32_t index = 0U; index < VFS_MOUNT_MAX; index++) {
		if (g_mounts[index].m_active) {
			uint32_t existing_length = vfs_string_length(g_mounts[index].m_path, VFS_PATH_MAX);
			if (existing_length == path_length) {
				bool equal = true;
				for (uint32_t offset = 0U; offset < path_length; offset++) {
					if (g_mounts[index].m_path[offset] != path[offset]) {
						equal = false;
						break;
					}
				}

				if (equal) {
					vfs_unlock(&g_vfs_lock);
					return VFS_STATUS_BUSY;
				}
			}
			continue;
		}

		if (mount == 0) mount = &g_mounts[index];
	}

	if (mount == 0) {
		vfs_unlock(&g_vfs_lock);
		return VFS_STATUS_NO_SPACE;
	}

	if (path_length != 1U && g_mount_count == 0U) {
		vfs_unlock(&g_vfs_lock);
		return VFS_STATUS_NOT_FOUND;
	}

	uint32_t slot = (uint32_t)(mount - g_mounts);
	memset(mount, 0, sizeof(*mount));
	mount->m_filesystem = filesystem;
	mount->m_device = device;
	mount->m_slot = slot;

	if (!vfs_copy_string(mount->m_path, VFS_PATH_MAX, path)) {
		memset(mount, 0, sizeof(*mount));
		vfs_unlock(&g_vfs_lock);
		return VFS_STATUS_PATH_TOO_LONG;
	}

	vfs_unlock(&g_vfs_lock);

	if (path_length != 1U) {
		vnode_t mountpoint;
		vfs_status_t lookup_status = vfs_lookup(path, &mountpoint);
		if (lookup_status != VFS_STATUS_OK) {
			memset(mount, 0, sizeof(*mount));
			return lookup_status;
		}

		bool directory = mountpoint->v_type == VNODE_TYPE_DIRECTORY;
		vnode_rele(mountpoint);
		if (!directory) {
			memset(mount, 0, sizeof(*mount));
			return VFS_STATUS_NOT_DIRECTORY;
		}
	}

	vfs_status_t status = filesystem->fs_ops->mount(filesystem, device, mount);
	if (status != VFS_STATUS_OK || mount->m_root == 0) {
		memset(mount, 0, sizeof(*mount));
		return status != VFS_STATUS_OK ? status : VFS_STATUS_IO_ERROR;
	}

	vfs_lock(&g_vfs_lock);
	mount->m_active = true;
	g_mount_count++;
	vfs_unlock(&g_vfs_lock);
	return VFS_STATUS_OK;
}


static bool vfs_path_equal(const char *left, const char *right)
{
	uint32_t left_length = vfs_string_length(left, VFS_PATH_MAX);
	uint32_t right_length = vfs_string_length(right, VFS_PATH_MAX);
	if (left_length != right_length) return false;
	for (uint32_t index = 0U; index < left_length; index++) {
		if (left[index] != right[index]) return false;
	}
	return true;
}

static bool vfs_mount_contains_mount(mount_t parent, mount_t child)
{
	if (parent == 0 || child == 0 || parent == child) return false;
	uint32_t parent_length = vfs_string_length(parent->m_path, VFS_PATH_MAX);
	uint32_t child_length = vfs_string_length(child->m_path, VFS_PATH_MAX);
	if (parent_length >= child_length) return false;

	if (parent_length == 1U && parent->m_path[0] == '/') return true;
	for (uint32_t index = 0U; index < parent_length; index++) {
		if (parent->m_path[index] != child->m_path[index]) return false;
	}
	return child->m_path[parent_length] == '/';
}

vfs_status_t vfs_sync_all(void)
{
	if (!g_vfs_initialized) return VFS_STATUS_INVALID;

	for (uint32_t index = 0U; index < VFS_MOUNT_MAX; index++) {
		mount_t mount = &g_mounts[index];
		if (!mount->m_active) continue;

		if (mount->m_filesystem == 0 || mount->m_filesystem->fs_ops == 0) return VFS_STATUS_IO_ERROR;
		if (mount->m_filesystem->fs_ops->sync == 0) continue;
		vfs_status_t status = mount->m_filesystem->fs_ops->sync(mount);
		if (status != VFS_STATUS_OK) return status;
	}

	return VFS_STATUS_OK;
}

vfs_status_t vfs_unmount(const char *path)
{
	if (!g_vfs_initialized || path == 0 || path[0] != '/') return VFS_STATUS_INVALID;
	uint32_t path_length = vfs_string_length(path, VFS_PATH_MAX);
	if (path_length == 0U || path_length == VFS_PATH_MAX) return VFS_STATUS_PATH_TOO_LONG;

	if (path_length > 1U && path[path_length - 1U] == '/') return VFS_STATUS_INVALID;

	mount_t target = 0;
	vfs_lock(&g_vfs_lock);
	for (uint32_t index = 0U; index < VFS_MOUNT_MAX; index++) {
		mount_t mount = &g_mounts[index];
		if (mount->m_active && vfs_path_equal(mount->m_path, path)) {
			target = mount;
			break;
		}
	}

	if (target == 0) {
		vfs_unlock(&g_vfs_lock);
		return VFS_STATUS_NOT_FOUND;
	}

	for (uint32_t index = 0U; index < VFS_MOUNT_MAX; index++) {
		if (g_mounts[index].m_active && vfs_mount_contains_mount(target, &g_mounts[index])) {
			vfs_unlock(&g_vfs_lock);
			return VFS_STATUS_BUSY;
		}
	}
	filesystem_t filesystem = target->m_filesystem;
	vfs_unlock(&g_vfs_lock);

	if (filesystem == 0 || filesystem->fs_ops == 0 || filesystem->fs_ops->unmount == 0) {
		return VFS_STATUS_NOT_SUPPORTED;
	}

	vfs_status_t status = filesystem->fs_ops->unmount(target);
	if (status != VFS_STATUS_OK) return status;

	vfs_lock(&g_vfs_lock);
	uint32_t slot = target->m_slot;
	memset(target, 0, sizeof(*target));
	target->m_slot = slot;
	if (g_mount_count != 0U) g_mount_count--;
	vfs_unlock(&g_vfs_lock);
	return VFS_STATUS_OK;
}

mount_t vfs_root_mount(void)
{
	for (uint32_t index = 0U; index < VFS_MOUNT_MAX; index++) {
		if (g_mounts[index].m_active && g_mounts[index].m_path[0] == '/' && g_mounts[index].m_path[1] == '\0') return &g_mounts[index];
	}

	return 0;
}

uint32_t vfs_mount_count(void)
{
	return g_mount_count;
}

uint32_t vfs_filesystem_count(void)
{
	return g_filesystem_count;
}

bool vfs_mount_get(uint32_t index, vfs_mount_info_t *info)
{
	if (!g_vfs_initialized || info == 0) return false;
	*info = (vfs_mount_info_t) { .device_index = UINT32_MAX };

	uint32_t current = 0U;
	vfs_lock(&g_vfs_lock);
	for (uint32_t slot = 0U; slot < VFS_MOUNT_MAX; slot++) {
		mount_t mount = &g_mounts[slot];
		if (!mount->m_active) continue;
		if (current++ != index) continue;

		if (mount->m_filesystem != 0) (void)vfs_copy_string(info->filesystem, sizeof(info->filesystem), mount->m_filesystem->fs_name);
		(void)vfs_copy_string(info->path, sizeof(info->path), mount->m_path);
		if (mount->m_device != 0) {
			(void)vfs_copy_string(info->device, sizeof(info->device), mount->m_device->name);
			info->read_only = mount->m_device->read_only;
			for (uint32_t device = 0U; device < block_device_count(); device++) {
				if (block_device_get(device) == mount->m_device) { info->device_index = device; break; }
			}
		} else {
			(void)vfs_copy_string(info->device, sizeof(info->device), "none");
		}

		vfs_unlock(&g_vfs_lock);
		return true;
	}

	vfs_unlock(&g_vfs_lock);
	return false;
}

vfs_status_t vfs_space_info(const char *path, vfs_space_info_t *info)
{
	if (!g_vfs_initialized || path == 0 || info == 0) return VFS_STATUS_INVALID;
	*info = (vfs_space_info_t) { 0 };

	mount_t mount;
	const char *relative;
	vfs_status_t status = vfs_path_select_mount(path, &mount, &relative);
	(void)relative;
	if (status != VFS_STATUS_OK) return status;
	if (mount->m_filesystem == 0 || mount->m_filesystem->fs_ops == 0 || mount->m_filesystem->fs_ops->space_info == 0) {
		return VFS_STATUS_NOT_SUPPORTED;
	}

	return mount->m_filesystem->fs_ops->space_info(mount, info);
}

const char *vfs_status_name(vfs_status_t status)
{
	switch (status) {
	case VFS_STATUS_OK: return "ok";
	case VFS_STATUS_INVALID: return "invalid";
	case VFS_STATUS_NOT_FOUND: return "not found";
	case VFS_STATUS_EXISTS: return "exists";
	case VFS_STATUS_NOT_DIRECTORY: return "not directory";
	case VFS_STATUS_IS_DIRECTORY: return "is directory";
	case VFS_STATUS_NO_MEMORY: return "no memory";
	case VFS_STATUS_NO_SPACE: return "no space";
	case VFS_STATUS_IO_ERROR: return "I/O error";
	case VFS_STATUS_READ_ONLY: return "read only";
	case VFS_STATUS_NOT_SUPPORTED: return "not supported";
	case VFS_STATUS_BAD_FD: return "bad fd";
	case VFS_STATUS_NAME_TOO_LONG: return "name too long";
	case VFS_STATUS_PATH_TOO_LONG: return "path too long";
	case VFS_STATUS_BUSY: return "busy";
	case VFS_STATUS_END_OF_DIRECTORY: return "end of directory";
	default: return "unknown";
	}
}

void vfs_dump(void)
{
	kputs("IOVirtualFSDriver filesystems: ");
	kputu64(g_filesystem_count);
	kputs(", mounts: ");
	kputu64(g_mount_count);
	kputc('\n');

	for (uint32_t index = 0U; index < VFS_MOUNT_MAX; index++) {
		mount_t mount = &g_mounts[index];
		if (!mount->m_active) continue;

		kputs("IOVirtualFSDriver mount ");
		kputs(mount->m_path);
		kputs(" type ");
		kputs(mount->m_filesystem->fs_name);
		kputs(", root vnode ");
		kputu64(mount->m_root->v_id);
		kputc('\n');
	}
}

/*
 * vfs_path_select_mount:
 *
 * Select the longest mount path which contains the absolute pathname. The
 * returned relative path begins at the first component inside that mount.
 */
vfs_status_t vfs_path_select_mount(const char *path, mount_t *result, const char **relative_path)
{
	if (result != 0) *result = 0;
	if (relative_path != 0) *relative_path = 0;
	if (path == 0 || result == 0 || relative_path == 0 || path[0] != '/') return VFS_STATUS_INVALID;

	uint32_t path_length = vfs_string_length(path, VFS_PATH_MAX);
	if (path_length == VFS_PATH_MAX) return VFS_STATUS_PATH_TOO_LONG;

	mount_t best = 0;
	uint32_t best_length = 0U;

	vfs_lock(&g_vfs_lock);

	for (uint32_t index = 0U; index < VFS_MOUNT_MAX; index++) {
		mount_t mount = &g_mounts[index];
		if (!mount->m_active) continue;

		uint32_t length = vfs_string_length(mount->m_path, VFS_PATH_MAX);
		if (length < best_length || length > path_length) continue;

		bool match = true;
		for (uint32_t offset = 0U; offset < length; offset++) {
			if (path[offset] != mount->m_path[offset]) {
				match = false;
				break;
			}
		}

		if (!match) continue;


		if (length != 1U && path[length] != '\0' && path[length] != '/') continue;

		best = mount;
		best_length = length;
	}

	vfs_unlock(&g_vfs_lock);

	if (best == 0) return VFS_STATUS_NOT_FOUND;

	const char *relative = path + best_length;
	while (*relative == '/') relative++;

	*result = best;
	*relative_path = relative;
	return VFS_STATUS_OK;
}
