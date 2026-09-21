#ifndef NXU_VFS_DEVFS_H
#define NXU_VFS_DEVFS_H

#include <vfs/vnode.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * devfs: the namespace of device nodes, mounted at /dev.
 *
 * A driver registers a named character device with a table of operations and
 * a context pointer; devfs shows it as a character vnode in a flat directory
 * and routes open, close, read, write and ioctl on that vnode to the table.
 * Devices are registered by drivers whenever they attach, which is usually
 * before the VFS exists: registration only fills a table, the vnodes are made
 * when the name is looked up after the mount.
 *
 * There is no permission model; who may open a device is the device's own
 * decision, made in its open operation (which can look at the calling process
 * with current_proc()).
 */

#define DEVFS_DEVICE_MAX 8U

typedef struct {
	/* All optional: an operation that is missing answers NOT_SUPPORTED (open and close succeed). */
	vfs_status_t (*open)(void *context, uint32_t flags);
	void (*close)(void *context);
	vfs_status_t (*read)(void *context, void *buffer, uint64_t size, uint64_t *read_size);
	vfs_status_t (*write)(void *context, const void *buffer, uint64_t size, uint64_t *written_size);
	vfs_status_t (*ioctl)(void *context, uint32_t command, void *argument);
} devfs_operations_t;

/*
 * devfs_register_device:
 *
 * Publish the character device `name` (one path component). False when the
 * name is empty, too long or taken, or the table is full.
 */
bool devfs_register_device(const char *name, const devfs_operations_t *operations, void *context);

/* How many devices are registered, and the name of one (0 past the end). */
uint32_t devfs_device_count(void);
const char *devfs_device_name(uint32_t index);

/* Publish the filesystem type to the VFS; mount it with vfs_mount("devfs", 0, "/dev"). */
bool devfs_register(void);

#endif
