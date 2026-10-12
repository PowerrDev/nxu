# NxuBoot

NxuBoot is NXU's own x64 UEFI bootloader. It opens `/NXU/kernel.elf` on its boot
volume, validates an ELF32 i386 executable, reserves its physical load range,
loads its segments, and hands off to the kernel in 32-bit protected mode with
paging disabled. QEMU's `-kernel` loader is not involved in this path.

The loader provides a Multiboot 1 memory map and framebuffer metadata, plus a
versioned NXU extension carrying the firmware RSDP. It prefers the ACPI 2.0
configuration-table entry. `/NXU/bootargs.txt` provides the kernel command line.
It obtains a fresh UEFI memory-map key immediately before ExitBootServices,
retries a stale key, then uses a reserved position-independent transition stub.
Firmware runtime services are not used after the handoff.

GOP RGB/BGR 32-bit linear framebuffers are supported. When no VirtIO display
binds, the kernel maps this framebuffer and presents from a software surface,
respecting its row pitch and channel order. Graphics acceleration is not supplied.

## Build on macOS

Clang, LLVM's `lld-link`/`ld.lld`, Python 3 and QEMU are required. The image builder
uses Python's standard library; it needs neither GNU filesystem utilities nor
`hdiutil`. Building NXU's separate ext4 system volume still uses e2fsprogs.

```sh
make nxuboot-image BUILD_ROOT=BUILD/BUILD-i386-desktop UISERVICE=0 WINDOWSERVER=0
```

This creates `BUILD/BUILD-i386-desktop/nxuboot/BOOTX64.EFI` and a 65 MiB GPT/FAT32
boot image, `nxu-boot.img`. Its EFI entry is `/EFI/BOOT/BOOTX64.EFI`. The image
contains the loader, kernel and boot arguments; it is separate from the ext4
system volume. `NXUBOOT_ARGS` overrides the command line embedded in the image.

## Run through firmware

```sh
make run-i386-desktop
make run-i386-recovery
```

These targets now boot NxuBoot through x64 UEFI by default. The legacy direct
path is available with `I386_BOOT=direct`. Intel macOS hosts use HVF; other hosts
use TCG. The x86 CPU's advertised physical-address width is limited to 32 bits,
and PCI's 64-bit aperture is disabled, so firmware places device BARs where the
current non-PAE kernel can map them.

For a lean recovery boot without the desktop Rust frameworks:

```sh
make run-i386-firmware BUILD_ROOT=BUILD/BUILD-i386-desktop UISERVICE=0 WINDOWSERVER=0
```

This builds a separate ext4 system volume and uses GOP graphics plus VirtIO
storage/input. `NXUBOOT_FIRMWARE` and `NXUBOOT_VARS` override the paired UEFI code
and variable templates supplied by QEMU. Each run uses a local copy of the
variable template. The ARM64 `make run` path is unchanged.

## Real hardware scope

The image is an unsigned UEFI boot prototype, not a laptop installer. Firmware
must permit its EFI application to execute. The kernel's fixed physical load
range must be available. NXU still needs drivers for the laptop's actual storage,
keyboard/trackpad and other devices to run its installed desktop. The boot
volume's firmware filesystem service stops being available after ExitBootServices.

The loader and firmware framebuffer path are tested in QEMU, including macOS
HVF; they have not been tested on the Dell laptop. No host disks or firmware boot
entries are modified by the build or run targets.

The i386 kernel presents the shared NXU boot splash as soon as VirtIO-GPU attaches,
or after registering the UEFI framebuffer fallback. The logo, progress bar and
boot log stay visible during startup and recovery-key selection. The splash
unregisters its console sink before recovery or the desktop takes over scanout.
