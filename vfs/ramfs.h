#ifndef NXU_VFS_RAMFS_H
#define NXU_VFS_RAMFS_H

#include <stdbool.h>

/*
 * ramfs_register:
 *
 * Publish the in-memory validation filesystem to VFS.
 */
bool ramfs_register(void);

#endif
