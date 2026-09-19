#ifndef NXU_VFS_VNODE_H
#define NXU_VFS_VNODE_H

#include <stdbool.h>
#include <stdint.h>

struct mount;
struct vnode;

typedef struct mount *mount_t;
typedef struct vnode *vnode_t;

#define VFS_DIRENT_NAME_MAX 255U

typedef enum {
	VNODE_TYPE_NONE,
	VNODE_TYPE_REGULAR,
	VNODE_TYPE_DIRECTORY,
	VNODE_TYPE_CHARACTER,
	VNODE_TYPE_BLOCK,
	VNODE_TYPE_SYMLINK,
	VNODE_TYPE_FIFO,
	VNODE_TYPE_SOCKET
} vnode_type_t;

typedef enum {
	VFS_STATUS_OK = 0,
	VFS_STATUS_INVALID,
	VFS_STATUS_NOT_FOUND,
	VFS_STATUS_EXISTS,
	VFS_STATUS_NOT_DIRECTORY,
	VFS_STATUS_IS_DIRECTORY,
	VFS_STATUS_NO_MEMORY,
	VFS_STATUS_NO_SPACE,
	VFS_STATUS_IO_ERROR,
	VFS_STATUS_READ_ONLY,
	VFS_STATUS_NOT_SUPPORTED,
	VFS_STATUS_BAD_FD,
	VFS_STATUS_NAME_TOO_LONG,
	VFS_STATUS_PATH_TOO_LONG,
	VFS_STATUS_BUSY,
	VFS_STATUS_END_OF_DIRECTORY
} vfs_status_t;

typedef struct {
	uint64_t inode;
	vnode_type_t type;
	uint32_t name_length;
	char name[VFS_DIRENT_NAME_MAX + 1U];
} vfs_dirent_t;

/*
 * vnode_attr_t
 *
 * What getattr reports about one vnode. va_mode carries the POSIX file type
 * bits (S_IFMT) together with the permission bits; times are seconds and
 * nanoseconds since the Unix epoch, zero when the filesystem records none.
 */
typedef struct {
	uint64_t va_inode;
	vnode_type_t va_type;
	uint32_t va_mode;
	uint32_t va_uid;
	uint32_t va_gid;
	uint32_t va_nlink;
	uint64_t va_size;
	uint64_t va_rdev;
	uint64_t va_atime_sec;
	uint32_t va_atime_nsec;
	uint64_t va_mtime_sec;
	uint32_t va_mtime_nsec;
	uint64_t va_ctime_sec;
	uint32_t va_ctime_nsec;
} vnode_attr_t;

typedef struct {
	vfs_status_t (*lookup)(vnode_t directory, const char *name, vnode_t *result);
	vfs_status_t (*create)(vnode_t directory, const char *name, vnode_type_t type, vnode_t *result);
	vfs_status_t (*unlink)(vnode_t directory, const char *name);
	vfs_status_t (*readdir)(vnode_t directory, uint64_t *offset, vfs_dirent_t *entry);
	vfs_status_t (*read)(vnode_t vnode, uint64_t offset, void *buffer, uint64_t size, uint64_t *read_size);
	vfs_status_t (*write)(vnode_t vnode, uint64_t offset, const void *buffer, uint64_t size, uint64_t *written_size);
	vfs_status_t (*truncate)(vnode_t vnode, uint64_t size);

	/* Optional: filesystems that do not provide them answer NOT_SUPPORTED. */
	vfs_status_t (*getattr)(vnode_t vnode, vnode_attr_t *attr);
	vfs_status_t (*readlink)(vnode_t vnode, char *buffer, uint64_t capacity, uint64_t *length);
} vnode_operations_t;

/*
 * struct vnode
 *
 * Filesystem-independent representation of one named object.
 *
 * The filesystem owns vnode storage and the object identified by v_data.
 * VFS owns only references to that storage. A vnode may remain resident at
 * reference count zero; reclamation policy belongs to the filesystem.
 *
 * v_mount
 *     Mount which supplied this vnode.
 *
 * v_parent
 *     Parent vnode used for path traversal of "..". The root vnode has no
 *     parent. The pointer is filesystem-resident and does not hold a VFS
 *     reference.
 *
 * v_ops
 *     Filesystem operations. Callers enter these routines only through the
 *     vnode wrappers below.
 */
struct vnode {
	mount_t v_mount;
	vnode_t v_parent;
	const vnode_operations_t *v_ops;
	void *v_data;

	uint64_t v_id;
	uint64_t v_size;

	uint32_t v_refcount;
	vnode_type_t v_type;

	bool v_active;
};

void vnode_init(
	vnode_t vnode,
	mount_t mount,
	vnode_t parent,
	const vnode_operations_t *operations,
	vnode_type_t type,
	uint64_t identifier,
	void *data
);

bool vnode_reference(vnode_t vnode);
void vnode_rele(vnode_t vnode);

vfs_status_t vnode_lookup(vnode_t directory, const char *name, vnode_t *result);
vfs_status_t vnode_create(vnode_t directory, const char *name, vnode_type_t type, vnode_t *result);
vfs_status_t vnode_unlink(vnode_t directory, const char *name);
vfs_status_t vnode_readdir(vnode_t directory, uint64_t *offset, vfs_dirent_t *entry);
vfs_status_t vnode_read(vnode_t vnode, uint64_t offset, void *buffer, uint64_t size, uint64_t *read_size);
vfs_status_t vnode_write(vnode_t vnode, uint64_t offset, const void *buffer, uint64_t size, uint64_t *written_size);
vfs_status_t vnode_truncate(vnode_t vnode, uint64_t size);
vfs_status_t vnode_getattr(vnode_t vnode, vnode_attr_t *attr);
vfs_status_t vnode_readlink(vnode_t vnode, char *buffer, uint64_t capacity, uint64_t *length);

#endif
