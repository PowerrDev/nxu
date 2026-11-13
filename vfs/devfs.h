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
 * Nodes come and go. A driver whose devices are created on demand (one
 * terminal per pseudo-terminal pair) registers and unregisters names while the
 * system runs. Unregistering removes the name; it does not revoke the files
 * already open on it, which keep reaching the driver's operations until they
 * are closed. The driver therefore keeps its context alive until the last
 * close it is told about, exactly as it must for any open device.
 *
 * A device with no ioctl operation answers every ioctl with NOT_TTY, as does
 * any other object that takes none: that is how isatty() learns the answer.
 *
 * Each node has an owner, a group and permission bits, reported by getattr
 * and enforced by the VFS at open (vnode_access). A device registered without
 * them belongs to root and is open to everyone (0666): whom to let in is then
 * the device's own decision, made in its open operation (which can look at
 * the calling process with current_proc()).
 */

#define DEVFS_DEVICE_MAX 64U

/* Permission bits of a device registered without any. */
#define DEVFS_MODE_DEFAULT 0666U

struct devfs_operations;

/*
 * devfs_clone_t
 *
 * How an open operation redirects the open to a device of its own making. A
 * cloning device (/dev/ptmx) is one name that gives every opener a different
 * device: its open fills in the operations and context of that new device and
 * the file being opened refers to it instead. The new device has no name; it
 * exists for as long as files are open on it and gets its own close.
 *
 * An open that leaves operations null opens the named device itself.
 */
typedef struct {
	const struct devfs_operations *operations;
	void *context;
} devfs_clone_t;

typedef struct devfs_operations {
	/* All optional: an operation that is missing answers NOT_SUPPORTED (open and close succeed). */
	vfs_status_t (*open)(void *context, uint32_t flags, devfs_clone_t *clone);
	void (*close)(void *context);
	/* ioflags is VNODE_IO_* for this one transfer (vfs/vnode.h). */
	vfs_status_t (*read)(void *context, void *buffer, uint64_t size, uint32_t ioflags, uint64_t *read_size);
	vfs_status_t (*write)(void *context, const void *buffer, uint64_t size, uint32_t ioflags, uint64_t *written_size);
	vfs_status_t (*ioctl)(void *context, uint32_t command, void *argument);
	/*
	 * What the device is ready for now (IOREADY_*, kern/ioready.h) and the
	 * notifier it posts to when that changes; must not sleep. A device whose
	 * reads or writes can wait provides it; one that does not is taken to be
	 * always ready. See the poll operation in vfs/vnode.h.
	 */
	vfs_status_t (*poll)(void *context, uint32_t *ready, struct ioready **source);
} devfs_operations_t;

/*
 * devfs_register_device:
 *
 * Publish the character device `name` (one path component), owned by root
 * with DEVFS_MODE_DEFAULT. False when the name is empty, too long or taken, or
 * the table is full.
 */
bool devfs_register_device(const char *name, const devfs_operations_t *operations, void *context);

/*
 * devfs_announce_registrations:
 *
 * Whether devfs_register_device() logs each node it publishes (it does by
 * default). The devfs self-test turns it off while it registers and drops its
 * scratch node over and over.
 */
void devfs_announce_registrations(bool announce);

/*
 * devfs_unregister_device:
 *
 * Remove `name` from the namespace. Files already open on it are unaffected
 * and still close through the device's operations; the name can be registered
 * again at once. False when there is no such device.
 */
bool devfs_unregister_device(const char *name);

/*
 * devfs_set_attributes:
 *
 * Give the device `name` an owner, a group and permission bits (the low nine
 * bits of mode). False when there is no such device.
 */
bool devfs_set_attributes(const char *name, uint32_t uid, uint32_t gid, uint32_t mode);

/* How many devices are registered, and the name of one (0 past the end). */
uint32_t devfs_device_count(void);
const char *devfs_device_name(uint32_t index);

/* Publish the filesystem type to the VFS; mount it with vfs_mount("devfs", 0, "/dev"). */
bool devfs_register(void);

/* Boot self-test: registration, attributes, cloning open and removal. Needs devfs mounted at /dev. */
bool devfs_self_test(void);

#endif
