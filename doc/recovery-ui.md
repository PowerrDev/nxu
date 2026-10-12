# sevOS recovery UI

sevOS has two intentionally separate graphical foundations.

```text
normal boot                         recovery boot
-----------                         -------------
UIService.framework                        Recovery.framework
    |                                   |
main sevOS applications             StartupOptionsUI
    |                                   |
main UI service                     triageOS (PID 1)
    |                                   |
normal display/input path           recovery-only syscall surface
```

The boundary is architectural, not a naming convention. `triageOS` does not
link UIService.framework and normal sevOS UI code does not link Recovery.framework.
The kernel accepts the direct recovery display and input syscalls only after
startup recovery mode has been selected.

## Entering triageOS

When a graphical display and input devices are available, the kernel shows the
startup splash for a short selection window. Hold **Shift+R** during that window
to request triageOS. On i386, the same chord is sampled during a three-second
selection window before userspace starts. You can also select recovery directly:

```sh
make run-i386-recovery
# ARM64: pass recovery=1 alongside your normal boot arguments
make run BOOT_ARGS="recovery=1"
```

The splash is kernel owned and deliberately minimal: the imported ArmOS startup
artwork and a clean progress bar. It does not display filesystem stage strings
such as `Mounting /`.

After storage is mounted, the selected environment becomes PID 1:

```text
normal     /System/Library/CoreServices/bootd
recovery   /System/Recovery/triageOS
```

Recovery boot skips the writable ext4 self-test so entering triageOS does not
create or modify the normal persistence marker as part of bootstrap.

## Recovery.framework

`frameworks/Recovery.framework` contains the recovery-only userspace stack:

```text
Recovery.framework/
├── include/Recovery/
│   ├── RecoveryServices.h
│   ├── StartupOptionsUI.h
│   └── drawing.h
├── lib/
│   └── RecoveryServices.c
├── StartupOptionsUI/
│   ├── drawing.c
│   ├── font_sans.c
│   ├── font_mono.c
│   └── font_ttf.c
└── triageOS/
    ├── main.c
    ├── shell.c
    └── ui.c
```

StartupOptionsUI is the renamed recovery descendant of ArmOS `libarmui`. It
uses process-owned ARGB buffers and the bootstrap raster fonts. It does not use
sevOS UIService.framework, WindowServer, or the main UI service.

`RecoveryServices` adapts the older triageOS service calls to the NXU userspace
ABI. Filesystem path metadata, seeked reads, directory creation, copying,
unlinking and sync use real NXU VFS operations.

## Current storage limitations

triageOS is built and staged for both ARM64 and i386. Terminal `rm` deletes
regular files; `rm -r` also removes nested files and empty ext4 directories.
Recovery directory enumeration hides `.` and `..`. Directory, read-only,
permission and nonempty-directory failures retain their specific diagnostics.
For protected directories, `rm -r --force-system PATH` opens the SAP modal and
requires the existing tepOS passcode before any deletion starts.

`partition-delete DEVICE INDEX --force` deletes an existing MBR/GPT partition.
The index is the zero-based index shown by `partitions`. A device with a mounted
filesystem returns `busy`; unmount it before changing its partition table.
The command does not erase the partition contents. The standard QEMU root
image contains raw ext4 and has no partition table to delete.

GPT initialization and partition creation still report `not supported`. Block
health, mount enumeration, filesystem space queries and verification are wired
to the kernel storage stack.
The Reinstall sevOS action explicitly reports that system-image installation is
not implemented.

triageOS is currently staged on the same ext4 system image as sevOS. A future
recovery-volume milestone should move `/System/Recovery` to an independently
bootable recovery filesystem before treating it as a corruption-resistant
recovery environment.

## UI assets

Cursor resources inherited from the ArmOS UI tree live in `tools/UI/Cursors`.
They belong to the normal external UIService.framework asset pipeline, not to
Recovery.framework. Apply them explicitly with:

```sh
make apply-assets
```

`UISERVICE_DIR` can override the sibling framework location:

```sh
make UISERVICE_DIR=../UIService.framework apply-assets
```

The target only copies resources; it does not make either UI foundation depend
on the other.

## Regression checks

`python3 tools/check_recovery_storage.py --arch i386` (or `--arch arm64`)
boots triageOS on private images, executes the real recovery shell and checks
file/recursive removal, mounted-device refusal, MBR/GPT partition deletion, read-only refusal and ext4
consistency. The i386 check also exercises Shift+R recovery selection.
