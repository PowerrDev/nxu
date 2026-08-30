# NXU root services

NXU's first long-lived userspace is implemented as real EL0 AArch64 ELF
programs rather than kernel threads. The kernel mounts the system disk, loads
`/System/Library/CoreServices/bootd`, maps its `PT_LOAD` segments into a new
process address space and requires it to receive PID 1.

## bootd

`bootd` is the root userspace service manager. PID 1 no longer contains a
hard-coded table of `logd` and `patchd`. It opens
`/disk/System/Library/BootDaemons`, enumerates `*.plist` definitions, validates
them through the CoreFoundation plist/service layers, and builds its job table
from those files.

`RunAtLoad` activates a job during root-service bootstrap. `KeepAlive` jobs are
reaped with nonblocking `waitpid` and restarted; five exits inside ten seconds
trigger a ten-second throttle. Safe mode and per-service boot-argument disable
rules are also declarative plist properties. See
[bootd service property lists](service-plists.md).

Before a service is launched, `bootd` compares the primary image against the
configured recovery image byte-for-byte. If they differ, the recovery image is
used. The kernel performs the same preflight for PID 1 and falls back to
`bootd.recovery` when the primary is missing, malformed, or different.

## logd

The kernel console retains a monotonic 32 KiB history. `logd` consumes that
history through the `klog_read` syscall, beginning at cursor zero so early boot
messages are replayed, and appends them to `/disk/var/log/system.log`.

## patchd

`patchd` periodically compares `bootd`, `logd`, and `patchd` to their staged
recovery images. Missing or different primaries are rewritten from recovery and
the filesystems are synchronized. Creating
`/disk/var/db/patchd/repair-all` requests an immediate repair pass; the request
file is removed after the pass.

This is recovery-copy integrity, not a cryptographic trust chain. NXU does not
yet provide signed manifests, authenticated snapshots, atomic rename, or an
SSV-style seal. Those are separate filesystem/security features.

## Security boundary

NXU does not yet have UID/GID credentials, vnode ownership/mode enforcement,
privilege transitions, or setuid semantics. These daemons execute in EL0 and
are isolated by process address spaces, but they are not yet separated by a
Unix root/non-root permission boundary. `root` and `sudo` should be added only
after credentials and VFS authorization exist; this implementation does not
fake them.
