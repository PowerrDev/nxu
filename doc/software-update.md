# NSW software update packages

An NXU Software Update (`.nsw`) file is an ISO 9660 container with Joliet names.
It has this layout:

```text
build-402.nsw
└── .nsw/
    ├── BuildManifest.plist
    ├── 402.img
    └── RecoveryRamdisk.img
```

Version 1 carries uncompressed raw ext4 images. DMG compression, multi-extent
ISO files and images/packages larger than 4 GiB are unsupported. The container
reader follows the volume descriptor and directory record structure in
[ECMA-119](https://ecma-international.org/publications-and-standards/standards/ecma-119/).
Joliet preserves `.nsw` and `BuildManifest.plist` exactly.

The manifest is a flat XML property list. Required fields are `FormatVersion`
(integer 1), `Build`, `Architecture` (`arm64` or `i386`), `SystemImage`,
`SystemImageSHA256`, `SystemImageFormat`, `RecoveryRamdisk`,
`RecoveryRamdiskSHA256` and `RecoveryRamdiskFormat`. Both formats are `ext4`;
SHA-256 values are lowercase hexadecimal. Payload names must be single safe
ASCII filename components. Unknown/duplicate keys and the wrong architecture
are rejected.

Create the recovery filesystem from the matching triageOS and kernel, then
package it with the system image:

```sh
python3 tools/make_recovery_image.py --arch arm64 --output recovery.img
python3 tools/make_nsw.py --build 402 --arch arm64 \
    --system-image tools/disk.img --recovery-ramdisk recovery.img \
    --output build-402.nsw
```

The builder uses macOS `hdiutil`, or `xorriso` on other hosts. It snapshots both
inputs before hashing and packaging, and refuses to overwrite an existing
output. The recovery-image builder stores triageOS, the kernel and
`RecoveryManifest.plist` in a raw ext4 filesystem. The package builder also
accepts an existing recovery filesystem image. The boot installer mounts these
images through a private read-only file-backed
block device. The recovery payload supplies triageOS when rollback cannot
complete; it runs with the current kernel and is not copied into RAM.

Place the package on the recovery volume at
`/Volumes/Recovery/mainOS/build-402.nsw`. In the recovery shell, use `disks` and
`partitions` to identify the recovery device, create `/Volumes` and
`/Volumes/Recovery` if needed, then mount its ext4 filesystem:

```text
mount ext4 DEVICE_INDEX /Volumes/Recovery
nsw prepare /Volumes/Recovery/mainOS/build-402.nsw
```

Recovery can prepare packages without starting patchd. In NZH, when the volume
is already mounted and the process has filesystem-write capability, use:

```text
patchd --prepare /Volumes/Recovery/mainOS/build-402.nsw
```

The running daemon also polls `/disk/var/db/patchd/prepare-update`. A request is
the full package path plus one terminating newline; append the newline only
when the request is complete. Daemon requests must name a single `.nsw` file
under `/Volumes/Recovery/mainOS`. The daemon consumes complete requests and
records `prepared` or `failed` in `/disk/var/db/patchd/update-status`. Incomplete
requests remain pending. It retains its existing daemon-integrity repair loop, logging the destination
of every repaired daemon.
For normal mainOS boots, `recovery.volume=DEVICE_INDEX` mounts the chosen ext4
block device/partition at `/Volumes/Recovery` before bootd starts. An unavailable
or already mounted device is refused without stopping mainOS. With no argument, the named GPT recovery partition is selected automatically
when present. Legacy raw secondary disks still require the override.

Preparation streams the images into `/Volumes/Recovery/Prepared/BUILD/`,
validates ISO bounds, manifest/schema, architecture and ext4 magic, checks both
SHA-256 hashes, syncs, and reads back the staged images to recheck their hashes.
It saves `BuildManifest.plist`, then writes and syncs `Ready` containing exactly
`verified\n`. A consumer must check its complete contents, not just existence.
An atomic `.prepare-lock` directory serializes writers on the recovery volume.
Normal completion removes it. Mounting the volume through `recovery.volume`
clears an empty lock before any updater starts. After a manual recovery mount,
if preparation was interrupted and no updater is running, use
`rmdir /Volumes/Recovery/Prepared/.prepare-lock` before retrying.
An already prepared build is immutable. Failed/interrupted preparation leaves
no usable ready marker (a torn marker is removed on retry) and can be retried in the same partial build directory.

## Recovery partition and desktop SSD

`make run-i386-desktop` now creates and boots
`BUILD/BUILD-i386-desktop/i386/ssd.img`. This is one GPT disk with two named
ext4 partitions: `NXU System` at `/disk` and `NXU Recovery` at
`/Volumes/Recovery`. The kernel discovers those partitions automatically;
no recovery device index or additional QEMU disk is needed. ARM64 supports the
same named GPT layout through its normal system-volume boot path.

The recovery partition includes a standalone
`System/Recovery/triageOS`, its `RecoveryRamdisk.img`, and a `mainOS` directory
for NSW packages. If the system filesystem cannot mount, NXU boots the
standalone recovery program from this partition. An invalid primary GPT map
can be read through its backup header/table. Ambiguous names and overlapping
system/recovery ranges are rejected.

The launcher retains an existing SSD across rebuilds. `system-template.img` contains the fresh build used to construct update
packages. On first creation, an existing legacy `disk.img` is copied into the
system partition; otherwise the new template is used. Neither that legacy
image nor the persistent guest SSD is rewritten by normal partitioned launches.
Factory recovery templates are also created once. The builders refuse to
replace existing SSD/recovery files. This creates a new image; it does not
repartition a host SSD or rewrite the existing raw system image.

Stop the VM before the first migration. To import the existing demo recovery
volume, including its NSW packages and prepared builds, run this once:

```sh
make run-i386-desktop I386_RECOVERY_DISK=BUILD/update-demo/recovery-volume.img
```

The argument seeds the recovery partition during its first creation; the disk
is then internal to `ssd.img`. Subsequent launches need only:

```sh
make run-i386-desktop
```

A seed containing `InstallPending` or `InstallReady` is refused, because an
active rollback journal must stay with its original system. Existing seeds
are read and copied into a new recovery filesystem. For a fresh install,
omit the seed argument even on the first launch.

For isolated debugging without tepOS, use:

```sh
make run-i386-desktop I386_BOOT=direct I386_DESKTOP_SMP=1 TEP=0 \
    BOOT_ARGS="dyrld.aslr=0"
```

Enter recovery with Shift+R during boot. Prepare and install an imported demo
package from its recovery shell:

```text
nsw prepare /Volumes/Recovery/mainOS/build-desktop-demo.nsw
nsw install desktop-demo
```

Already prepared builds do not need preparation again. Avoid a persistent
`recovery=1` boot argument for installation: the reboot should enter normal
mainOS so the offline installer runs. Keep the system running until patchd
reports boot confirmation. A restart before confirmation rolls back the
update, including a newly added `UpdateDemo.txt`.

Dock-launched Terminal currently has no filesystem-write capability, so direct
preparation or writing daemon request files there is denied. Recovery runs as
PID 1 and can perform these operations. Package extraction now reports progress
on serial output and announces the read-back verification phase; it still
blocks the recovery UI until preparation completes.

Legacy raw-disk development remains available through
`I386_PARTITIONED_DISK=0`; in that mode `I386_RECOVERY_DISK` attaches the old
separate recovery disk and supplies `recovery.volume=1`.

For custom images, use `tools/make_recovery_volume.py` to create a recovery
filesystem from a ramdisk, optional seed and optional `--package` inputs, then
`tools/make_system_disk.py --system-image SYSTEM.img --recovery-image RECOVERY.img
--output SSD.img`. These tools create files only; they do not modify a physical
disk's partition table. The firmware/EFI boot medium remains separate from the
system/recovery SSD image.

## Automatic installation

After preparation, select the build from a privileged shell:

```text
patchd --install 402
```

In triageOS, use `nsw install 402`. The command verifies the prepared manifest
and both images again, queues the build, syncs the queue and reboots. If the
calling shell cannot reset the machine, the queue remains for the next reboot.
For the running patchd service, write `402` plus a terminating newline to
`/disk/var/db/patchd/install-update`. Patchd consumes that request and reboots
without another confirmation prompt. Its service plist grants filesystem-write
and reset capabilities. Merely placing or preparing a package never schedules
installation.

The installer runs before userland on ARM64 and i386. It verifies the queued
manifest digest, payload hashes, architecture, readable ext4 filesystems and
recovery triageOS ELF. It plans regular files and directories below `System`,
`Applications`, `bin` and `init`. Symlinks, other node types, incompatible
existing types and oversized plans are rejected before system files change.
Version 1 permits at most 1,024 entries and paths fitting the VFS path limit.
The three core daemons must have matching `.recovery` copies in the payload. Image mounts require a clean supported ext4 filesystem
with an internal journal and metadata checksums.

The installer preserves `/disk/Users`, `/disk/var` and existing files absent
from the selected system image. It updates the mounted system filesystem;
external EFI boot media and the running kernel boot image are not replaced.
The kernel included in the recovery image is retained as a recovery artifact,
not loaded by this workflow.

Before replacing any file, the installer copies originals to
`/Volumes/Recovery/Transactions/BUILD-NONCE/Backup/` and syncs a bounded,
architecture-independent journal. It logs directories and each changed file:

```text
patchd: backing up /disk/System/Library/CoreServices/bootd
patchd: applying patches to /disk/System/
patchd: applying patches to /disk/System/Library/CoreServices/bootd
patchd: applying patches to /disk/Applications/Terminal.app/Contents/SevOS/Terminal
```

Each copied file is checked against its SHA-256 digest. Copy or sync failures
trigger rollback. An interrupted or unconfirmed journalled update rolls back
before userland on the following boot: original files are restored and new
files/directories removed. Rollback logs each changed destination too. If a
backup is missing/corrupt or rollback fails, the boot path re-verifies and
mounts the recovery payload and launches its triageOS; it refuses a partial
system boot when that recovery path is also unavailable.

A completed installation enters a trial boot. After running for 30 seconds,
patchd checks the three core daemon images against their recovery copies and
marks the transaction committed. A kernel watchdog reboots after 90 seconds
without confirmation so the next boot restores the old files. This is a core
integrity/liveness check, not a complete application or desktop health test.
Booting with patchd disabled leaves a trial unconfirmed. Backups are retained
after commit; this version does not automatically prune transaction storage.

`InstallPending` records the build, unique transaction and exact manifest
hash. `InstallReady` is published only after the pending record is synced.
Only an explicitly requested local installation is authorized. Package hashes
detect corruption; they do not authenticate an update publisher. Network
updates and publisher-signature verification are not implemented.

Run `python3 tools/check_nsw.py` for sanitizer-backed extraction and rejection
tests with real ISO images, short I/O, corrupt payloads and write/sync failures.
`python3 tools/check_nsw_vm.py --arch i386` checks native i386 extraction.

Run `python3 tools/check_update_vm.py --partitioned --arch arm64 --case success` for queue,
automatic reboot, installation, daemon confirmation and a second boot. Repeat
with `--arch i386`. Cases `failure`, `interrupted`, `unconfirmed`, `tamper` and
`recovery` exercise copy-failure rollback, power loss during copying, an
interrupted trial, manifest rejection and verified recovery fallback. Case
`core` replaces all three daemon images and their recovery copies;
`core-unconfirmed` verifies restoration of those binaries. Cases `incomplete`,
`unsupported` and `watchdog` check torn queue publication, symlink rejection
and automatic reboot of an unconfirmed boot. These checks use separate system
and recovery volumes, assert per-file logs and verify preservation of user/state
files. For parallel VM cases, build once and add `--no-build`.

`--case partition-recovery --partitioned` corrupts the system filesystem and
checks booting the standalone recovery partition. `--damage-primary-gpt` checks
GPT discovery through the backup map.
