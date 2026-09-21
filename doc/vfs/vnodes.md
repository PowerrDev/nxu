# Vnodes

## Overview

A vnode is the filesystem-independent kernel object used to represent a file,
directory or device node.

`struct vnode` is deliberately smaller than an on-disk inode. Filesystems keep
format-specific state behind `v_data` and expose only operations needed by VFS.

## Object state

A vnode records:

```text
mount
parent
operation vector
filesystem-private data
stable filesystem identifier
size
type
reference count
active state
```

The current vnode types are:

```text
regular
directory
character
block
```

Only regular files and directories are created by the current VFS APIs; character
vnodes are the device nodes of [devfs](devfs.md).

## Operation vector

`vnode_operations_t` contains:

```text
lookup
create
unlink
read
write
truncate
```

Device vnodes also use three optional operations, `open`, `close` and `ioctl`
(see [devfs](devfs.md)); a filesystem that does not provide them opens and closes
freely and answers `NOT_SUPPORTED` to `ioctl`.

The wrappers in `vnode.c` enforce common type and argument checks before
entering filesystem code.

Directory lookup, creation and unlink are rejected on non-directory vnodes.

Read, write and truncate are rejected on directory vnodes.

## Ownership

Vnode storage belongs to the filesystem.

VFS references do not determine whether the filesystem object itself exists.
A resident filesystem may keep a vnode allocated with no transient VFS
references.

The initial reference created by `vnode_init()` is therefore a filesystem
residency reference.

`vnode_reference()` and `vnode_rele()` manage transient kernel ownership above
that residency reference.

## Parent pointers

`v_parent` is used by the pathname resolver for `..`.

The pointer is filesystem-resident and does not take an additional VFS
reference. The filesystem must keep the parent object valid while the child is
reachable.

A mount root has no parent within that mount.

## Size

`v_size` is the VFS-visible logical size of the object.

The backing filesystem updates it as file data changes. VFS does not infer
size from block allocation or transport geometry.

## Locking

The vnode reference count uses atomic operations.

Object-content serialization belongs to the filesystem. VFS does not hold a
global vnode lock while invoking filesystem operations.

## Invariants

- an active vnode has a valid operation vector;
- vnode storage outlives every VFS reference to it;
- lookup and create return referenced vnodes;
- a file object holds exactly one vnode reference;
- filesystem-private data is interpreted only by the owning filesystem.
