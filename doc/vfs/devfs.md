# devfs

## Overview

devfs (`vfs/devfs.c`) is the namespace of device nodes, mounted at `/dev`. It is a
flat directory of character vnodes; there are no subdirectories and no creation
or removal from user space. Each node has an owner, a group and permission bits,
which the VFS enforces at open like any file's; beyond that, who may open a
device is the device's own decision, made in its `open`.

## Registering a device

A driver publishes a device with

```c
devfs_register_device("audio0", &operations, context);
```

`devfs_operations_t` holds optional `open`, `close`, `read`, `write` and `ioctl`
routines, each given the driver's context pointer. An operation that is missing
answers `NOT_SUPPORTED` (`open` and `close` succeed). Registration only fills a
table (up to 64 devices, names of one path component), so a driver can register
when it attaches, which is before the VFS exists; the vnode for a name is made the
first time it is looked up after the mount, and stays resident.

### Nodes that come and go

`devfs_unregister_device(name)` removes a name while the system runs. It does
not revoke anything: files already open on the device keep reaching its
operations until they are closed, so the driver keeps its context alive until
the last `close` it is told about. The name can be registered again at once.

A slot is *free*, *named*, or *detached* (in use but nameless, because it was
unregistered while open or was made by a cloning open). Vnodes are embedded in
slots and `vnode_rele` has no last-reference callback, so a detached slot is
reclaimed lazily: the next time a slot is wanted, any detached slot whose vnode
is back at its residency reference is freed. Nothing can gain a reference to a
detached device in between, since it has no name. One spinlock, a leaf never
held across a driver operation, protects the table.

### Ownership and mode

A device registered with `devfs_register_device` belongs to root with mode
`0666`, which leaves the decision to its `open` as before.
`devfs_set_attributes(name, uid, gid, mode)` changes that from the kernel;
`chmod` and `chown` change it from user space through `setattr`. `stat` reports
`S_IFCHR`, the owner and the mode.

### Cloning devices

`open(context, flags, clone)` may redirect the open to a device of its own
making by filling in `clone->operations` and `clone->context`. devfs gives that
device a detached slot (starting with the attributes of the device that made
it) and the file is opened on it instead: every opener of the one name gets a
different device, which is closed through its own `close`. The cloning device's
own `close` is not called for a redirected open. If no slot is free, the new
device's `close` is called so the driver can release it, and the open fails
with `NO_SPACE`. This is what `/dev/ptmx` is built on.

The mount happens where `/Volumes/mainOS`'s mount point is made: `kern_init_filesystem()` on
arm64 and `i386_init_kernel()` on i386 register the filesystem, make `/dev` in the
ramfs root and mount `devfs` on it.

## What the VFS gained for it

`vnode_operations_t` has three optional operations (see [Vnodes](vnodes.md)):

- `open(vnode, flags, &opened)` runs in `vfs_open` after the path is resolved and
  before a file object is allocated. It may refuse the open, and a refusal
  leaves nothing behind. It may also store a different, referenced vnode in
  `opened`, and the file is then opened on that vnode instead. devfs uses it to let a device be exclusive and to check the caller's
  capability (`current_proc()`).
- `close(vnode, flags)` runs once, when the last reference to the open file is
  released. A descriptor shared by `fork` closes the device when the last holder
  goes, and a process that exits with the device open closes it in
  `filedesc_close_all()`.
- `ioctl(vnode, command, argument)`. The `ioctl` system call
  (`NXU_SYS_IOCTL`, `nxu_ioctl(fd, command, arg)`) decodes the command: it
  encodes the direction of the argument (`NXU_IOC_IN`, `NXU_IOC_OUT`), its size (at
  most 256 bytes) and a device group and number, so the system call copies the
  argument in and out and the device sees a kernel buffer.

Three statuses were added for devices: `VFS_STATUS_WOULD_BLOCK`
(`-NXU_SYS_E_AGAIN`), `VFS_STATUS_INTERRUPTED` (`-NXU_SYS_E_INTERRUPTED`) and
`VFS_STATUS_DENIED` (`-NXU_SYS_E_DENIED`), and `VFS_OPEN_NONBLOCK`
(`NXU_O_NONBLOCK`).

`read` and `write` receive `ioflags` (`VNODE_IO_*`, `vfs/vnode.h`) describing
that one transfer. `VNODE_IO_NONBLOCK` is set when the file was opened
`VFS_OPEN_NONBLOCK`, or when the caller asked not to wait this once
(`nxu_read_poll`); a device that would have to sleep answers `WOULD_BLOCK`, or
a short count if it moved part of the data first. The flag belongs to the
transfer, so two files open on one device may differ. A device with a single
opener may still fix its mode at `open`, as `/dev/audio0` does.

A device whose reads or writes can wait also provides `poll(context, &ready,
&source)`: what it is ready for now (`IOREADY_*`) and the notifier
(`kern/ioready.h`) it posts to when that changes. A device without `poll` is
taken to be always ready.

A device is a stream: the `read` system call asks it once and returns what it
gave, and `ioctl` works on any descriptor, 0 to 2 included.

`open` for writing of an existing character device does not need
`NXU_CAP_FS_WRITE` (it does not change the filesystem); create, truncate and append
still do. The audio device has its own capability, `NXU_CAP_AUDIO`.

## Current devices

| Node | Driver | Notes |
| --- | --- | --- |
| `/dev/audio0` | VirtIO Sound | see [VirtIO Sound](../drivers/virtio-sound.md) |

## Limitations

`devfs_self_test()` runs at every boot once `/dev` is mounted and covers
registration, attributes, cloning open, per-call and per-open nonblocking reads,
removal while open, and slot reclamation.

No `mknod`, no subdirectories, no nodes for the existing input or block devices,
no unmount of a busy device, no revocation of open files when a device is
unregistered, and `read` and `seek` on a device are up to the device (`/dev/audio0` has
no `read`).
