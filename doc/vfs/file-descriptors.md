# Files and Descriptor Tables

## Overview

Vnodes identify filesystem objects. File objects identify opens of those
objects.

This distinction keeps seek offsets and open flags per open instance rather
than per vnode.

```text
process
  |
  v
filedesc
  |
  +-- fd 0 -> file -> vnode
  +-- fd 1 -> file -> vnode
  `-- fd n -> file -> vnode
```

## File objects

`struct file` contains:

```text
vnode reference
current offset
open flags
reference count
global file-table slot
active state
```

NXU currently provides 128 global file objects.

A new file object takes one vnode reference. The final file reference releases
that vnode reference and returns the file slot to the global table.

## Descriptor tables

Every `struct proc` embeds one `struct filedesc` as `p_fd`.

A descriptor is an index into `fd_ofiles`.

The current table contains 32 descriptors per process.

New opens use the lowest available descriptor at or after `fd_freefile`.
Closing a lower descriptor moves the allocation hint backward so it can be
reused.

## Reference rules

Installing a newly allocated file transfers its initial file reference into
the descriptor table.

`filedesc_get()` takes a temporary file reference before returning the object
to a caller.

`vfs_read()`, `vfs_write()` and `vfs_seek()` release that temporary reference
before returning.

`vfs_close()` removes the descriptor and releases the descriptor-owned
reference.

Process exit calls `filedesc_close_all()` before the proc enters zombie state.
A zombie therefore retains process identity and exit status but no open file
references.

## File offsets

Read and write operations use `f_offset`.

A successful read advances it by the number of bytes returned.

A successful ordinary write advances it by the number of bytes written.

An append write ignores the previous offset for placement, starts at the
vnode's current size, then updates the file offset to the end of the write.

`vfs_seek()` currently supports only assignment of an absolute offset.

## Locking

The global file table has one lock protecting allocation and file reference
counts.

Each descriptor table has an independent lock protecting descriptor slots,
open count and allocation hint.

No descriptor-table lock is held while invoking vnode I/O.

## Current limitations

`fork` (arm64) gives the child every descriptor of the parent, each pointing at
the same file object, so parent and child share file offsets
(`filedesc_fork`). `exec` keeps all descriptors open. See
[Process control](../kern/process-control.md).

There is no `dup`, `fcntl`, close-on-exec state, credentials, advisory locking,
or userspace open/read/close syscall interface yet.
