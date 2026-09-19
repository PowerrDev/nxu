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
to request triageOS.

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

The triageOS interface is preserved, including Terminal and Disk Utility, but
NXU does not yet contain the partition-management and recovery-volume stack
that old ArmOS had. GPT creation, partition creation/deletion, block health,
mount enumeration and filesystem space queries therefore report `not supported`.
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
