# devfs

## Overview

devfs (`vfs/devfs.c`) is the namespace of device nodes, mounted at `/dev`. It is a
flat directory of character vnodes; there are no subdirectories, no creation or
removal from user space, and no permission model (who may open a device is the
device's own decision, made in its `open`).

## Registering a device

A driver publishes a device with

```c
devfs_register_device("audio0", &operations, context);
```

`devfs_operations_t` holds optional `open`, `close`, `read`, `write` and `ioctl`
routines, each given the driver's context pointer. An operation that is missing
answers `NOT_SUPPORTED` (`open` and `close` succeed). Registration only fills a
table (up to 8 devices, names of one path component), so a driver can register
when it attaches, which is before the VFS exists; the vnode for a name is made the
first time it is looked up after the mount, and stays resident.

The mount happens where `/disk`'s mount point is made: `kern_init_filesystem()` on
arm64 and `i386_init_kernel()` on i386 register the filesystem, make `/dev` in the
ramfs root and mount `devfs` on it.

## What the VFS gained for it

`vnode_operations_t` has three optional operations (see [Vnodes](vnodes.md)):

- `open(vnode, flags)` runs in `vfs_open` after the path is resolved and before a
  file object is allocated. It may refuse the open, and a refusal leaves nothing
  behind. devfs uses it to let a device be exclusive and to check the caller's
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
(`NXU_O_NONBLOCK`), which the device reads from the flags it is opened with.

`open` for writing of an existing character device does not need
`NXU_CAP_FS_WRITE` (it does not change the filesystem); create, truncate and append
still do. The audio device has its own capability, `NXU_CAP_AUDIO`.

## Current devices

| Node | Driver | Notes |
| --- | --- | --- |
| `/dev/audio0` | VirtIO Sound | see [VirtIO Sound](../drivers/virtio-sound.md) |

## Limitations

No `mknod`, no nodes for the existing input or block devices, no unmount of a busy
device, and `read` and `seek` on a device are up to the device (`/dev/audio0` has
no `read`).
