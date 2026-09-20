# Virtual Filesystem

## Overview

The virtual filesystem provides the kernel namespace above individual
filesystem implementations.

Callers operate on vnodes, files and descriptor tables. They do not depend on
ramfs, ext4, VirtIO Block or any other backing store.

```text
pathname
   |
   v
mount table
   |
   v
vnode
   |
   +-- filesystem operations
   |
   v
file
   |
   v
per-process descriptor table
```

The current implementation is intentionally small, but the object boundaries
are permanent. ramfs and ext4 both attach below the same mount and vnode
interfaces.

## Filesystem registration

A `struct vfs_filesystem` describes one filesystem type.

Registration publishes the filesystem name and its mount operations. It does
not create filesystem state and it does not attach storage.

The registry currently holds at most eight filesystem types.

`ramfs`, writable `ext4` and `devfs` (the device nodes, mounted at `/dev`; see
[devfs](devfs.md)) are the filesystem types registered at boot.

## Mounts

A `struct mount` represents one filesystem attachment.

A mount owns:

```text
filesystem type
optional block device
root vnode
filesystem-private mount state
namespace path
```

The global mount table contains at most eight active mounts.

Path lookup selects the longest mount pathname that contains the requested
absolute path. The remaining components are then resolved from that mount's
root vnode.

The bootstrap namespace is:

```text
/      -> ramfs
/dev   -> devfs
/disk  -> ext4 on disk0
```

The ext4 mount is attached after VirtIO Block discovery. Nested mounts are
represented by the same mount table. `vfs_unmount()` removes an exact mount
path only when it has no child mounts and the filesystem reports that no live
references prevent teardown.

## Pathname lookup

Only absolute paths are accepted.

A path is resolved one component at a time. Intermediate vnode references are
released before traversal continues.

The resolver understands:

```text
.
..
repeated '/'
```

`..` follows the vnode's resident parent pointer and stops at the root of the
selected mount.

Pathnames are limited to 255 characters plus the terminating NUL. Individual
components are limited to 63 characters plus NUL.

## Namespace mutation

`vfs_unlink()` resolves the parent directory and delegates regular-file removal
to the filesystem vnode operation. Directory removal remains separate.

## Creation

`vfs_create()` resolves the parent directory and calls the filesystem's vnode
create operation for the final component.

The current public creation types are regular files and directories.

`vfs_mkdir()` is a directory-specific wrapper around the same path.

## File access

`vfs_open()` resolves or creates a vnode and allocates a distinct file object.

Supported open flags are:

```text
VFS_OPEN_READ
VFS_OPEN_WRITE
VFS_OPEN_CREATE
VFS_OPEN_TRUNCATE
VFS_OPEN_APPEND
```

Every file object owns its own current offset.

Read and write operations advance that offset only after a successful vnode
operation. Append writes take the vnode's current size as the write offset.

## Synchronization and teardown

`vfs_sync_all()` walks every active mount and asks the filesystem to make its
current state durable. A filesystem without persistent backing may implement
sync as a no-op; ext4 validates that no JBD2 transaction is in flight and then
flushes the block device.

`vfs_unmount()` first rejects mounts with nested child mounts. The filesystem
then performs its own busy-reference checks and teardown. ext4 keeps one
residency reference on every cached vnode and one additional mount reference on
its root vnode; any reference beyond those internal owners makes unmount return
`VFS_STATUS_BUSY`.

This gives NXU an explicit durability boundary before filesystem work is
frozen and higher-level user interfaces begin depending on persistent state.

## Initialization

VFS is initialized after the kernel heap and process manager are available.

The boot sequence registers ramfs, mounts it at `/`, and runs a namespace and
file-descriptor self-test before thread scheduling begins.

The validation creates `/tmp/hello`, writes a known payload, seeks to offset
zero, reads it back, closes the descriptor, and resolves a pathname containing
both `.` and `..`.

## Locking

The filesystem registry and mount table have one VFS lock.

The global open-file table has a separate lock.

Each process descriptor table has its own lock.

Filesystem-private locking belongs to the filesystem implementation. ramfs
uses one lock per mount.

No VFS lock is held across filesystem vnode operations.

## Invariants

The following rules must remain true:

- filesystem implementations own vnode storage;
- an open file owns one vnode reference;
- a descriptor table owns one file reference for each installed descriptor;
- pathname lookup returns a referenced vnode;
- filesystem code never reaches into a process descriptor table;
- VFS code never depends on a concrete block transport;
- a process closes descriptor-owned file references during exit.

## Current limitations

There is no rename, hard-link creation, symbolic-link, directory removal,
directory enumeration, permissions model, current working directory, descriptor
duplication, polling, or userspace file syscall interface yet.

The ext4 writer uses synchronous ordered data writes and JBD2 metadata
transactions. The journal is checkpointed immediately after each normal commit;
asynchronous writeback and batched transactions are not implemented.
