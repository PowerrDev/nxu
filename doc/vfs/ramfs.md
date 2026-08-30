# ramfs

## Overview

ramfs is the bootstrap in-memory filesystem used to validate VFS independently
of block storage and ext4.

It is not intended to become the persistent NXU filesystem.

A ramfs mount owns an in-memory directory tree. Nodes and file contents are
allocated from the kernel heap and remain resident until the mount is
destroyed.

## Node layout

Each ramfs node contains an embedded vnode plus filesystem-private state:

```text
vnode
parent
first child
next sibling
name
data pointer
data capacity
```

Directories use the child and sibling links.

Regular files use the data pointer and capacity fields.

## Directory lookup

Directory entries are stored as a singly linked list of children.

Lookup is therefore linear in the number of children in that directory.

This is sufficient for VFS validation and intentionally avoids adding an index
that ext4 will not use.

## File storage

Regular-file data begins with no backing allocation.

The first write allocates at least 256 bytes. Capacity then grows geometrically
until the requested range fits.

A single ramfs file is currently limited to 1 MiB.

Newly exposed bytes are zero-filled when a write or truncate creates a hole
between the old end of file and the new size.

## Vnode residency

Every ramfs node embeds a vnode and keeps the initial vnode reference as its
filesystem residency reference.

Lookup and create take an additional transient reference for the caller.

A transient reference reaching zero does not reclaim the node. The directory
tree remains authoritative until unmount.

## Locking

One lock protects each ramfs mount.

The lock serializes directory mutation, lookup, file growth, reads, writes and
truncate operations.

Heap allocation may occur while this lock is held. The current kernel does not
preempt ordinary EL1 execution, so this is acceptable for the bootstrap
filesystem. This policy should be revisited before general kernel preemption or
SMP.

## Boot validation

The VFS boot test creates:

```text
/
`-- tmp/
    `-- hello
```

It writes `NXU VFS ramfs self-test`, reads the payload through the same file
descriptor, then resolves:

```text
/tmp/./../tmp/hello
```

The test proves vnode lookup, creation, file offsets, descriptor ownership and
`.`/`..` traversal without involving VirtIO Block.

## Current limitations

ramfs has no removal, rename, directory enumeration, links, permissions,
timestamps, sparse-allocation accounting or persistence.

Persistent storage belongs to ext4 below the same VFS interface.
