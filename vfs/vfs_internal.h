#ifndef NXU_VFS_VFS_INTERNAL_H
#define NXU_VFS_VFS_INTERNAL_H

#include <vfs/vfs.h>

vfs_status_t vfs_path_select_mount(const char *path, mount_t *mount, const char **relative_path);
vfs_status_t vfs_lookup_parent(const char *path, vnode_t *parent, char name[VFS_NAME_MAX + 1U]);

#endif
