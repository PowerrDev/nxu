#ifndef NXU_VFS_VFS_H
#define NXU_VFS_VFS_H

#include <drivers/block/block_device.h>
#include <vfs/file.h>
#include <vfs/vnode.h>

#include <stdbool.h>
#include <stdint.h>

#define VFS_FILESYSTEM_MAX 8U
#define VFS_MOUNT_MAX 8U
#define VFS_PATH_MAX 256U
#define VFS_NAME_MAX 63U
#define VFS_FILESYSTEM_NAME_MAX 31U

struct vfs_filesystem;
struct mount;

typedef struct vfs_filesystem *filesystem_t;
typedef struct mount *mount_t;

typedef struct {
	uint64_t total_bytes;
	uint64_t free_bytes;
	uint32_t block_size;
	bool read_only;
} vfs_space_info_t;

typedef struct {
	char filesystem[VFS_FILESYSTEM_NAME_MAX + 1U];
	char path[VFS_PATH_MAX];
	char device[BLOCK_DEVICE_NAME_MAX + 1U];
	uint32_t device_index;
	bool read_only;
} vfs_mount_info_t;

typedef struct {
	vfs_status_t (*mount)(filesystem_t filesystem, block_device_t device, mount_t mount);
	vfs_status_t (*sync)(mount_t mount);
	vfs_status_t (*unmount)(mount_t mount);
	vfs_status_t (*space_info)(mount_t mount, vfs_space_info_t *info);
} filesystem_operations_t;

/*
 * struct vfs_filesystem
 *
 * Registered filesystem implementation.
 *
 * Registration publishes only the filesystem type. Per-mount state belongs
 * to struct mount and is created by the filesystem mount operation.
 */
struct vfs_filesystem {
	char fs_name[VFS_FILESYSTEM_NAME_MAX + 1U];
	const filesystem_operations_t *fs_ops;
	bool fs_registered;
};

/*
 * struct mount
 *
 * One attachment of a filesystem into the global namespace.
 *
 * m_root is held by the filesystem for the lifetime of the mount. m_data is
 * private filesystem state. VFS owns the pathname and mount-table slot.
 */
struct mount {
	filesystem_t m_filesystem;
	block_device_t m_device;
	vnode_t m_root;
	void *m_data;

	char m_path[VFS_PATH_MAX];
	uint32_t m_slot;
	bool m_active;
};

bool vfs_init(void);
bool vfs_register_filesystem(filesystem_t filesystem);

vfs_status_t vfs_mount(const char *filesystem_name, block_device_t device, const char *path);
vfs_status_t vfs_sync_all(void);
vfs_status_t vfs_unmount(const char *path);

vfs_status_t vfs_lookup(const char *path, vnode_t *result);
vfs_status_t vfs_create(const char *path, vnode_type_t type, vnode_t *result);
vfs_status_t vfs_mkdir(const char *path);
vfs_status_t vfs_unlink(const char *path);

vfs_status_t vfs_open(filedesc_t filedesc, const char *path, uint32_t flags, uint32_t *descriptor);
vfs_status_t vfs_close(filedesc_t filedesc, uint32_t descriptor);
vfs_status_t vfs_read(filedesc_t filedesc, uint32_t descriptor, void *buffer, uint64_t size, uint64_t *read_size);
vfs_status_t vfs_readdir(filedesc_t filedesc, uint32_t descriptor, vfs_dirent_t *entry);
vfs_status_t vfs_write(filedesc_t filedesc, uint32_t descriptor, const void *buffer, uint64_t size, uint64_t *written_size);
vfs_status_t vfs_seek(filedesc_t filedesc, uint32_t descriptor, uint64_t offset);
vfs_status_t vfs_ioctl(filedesc_t filedesc, uint32_t descriptor, uint32_t command, void *argument);

mount_t vfs_root_mount(void);
uint32_t vfs_mount_count(void);
uint32_t vfs_filesystem_count(void);
bool vfs_mount_get(uint32_t index, vfs_mount_info_t *info);
vfs_status_t vfs_space_info(const char *path, vfs_space_info_t *info);

const char *vfs_status_name(vfs_status_t status);
void vfs_dump(void);

#endif
