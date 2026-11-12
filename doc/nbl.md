# NBL

NBL, the NXU Boot Loader, is NXU's own UEFI boot loader (`boot/uefi`). It is one
application built per firmware architecture:

| Firmware | Binary | Kernels it loads |
| --- | --- | --- |
| x64 | `BOOTX64.EFI` | the i386 Multiboot kernel, the x86-64 kernel |
| AArch64 | `BOOTAA64.EFI` | the arm64 kernel |

It opens `/NXU/kernel.elf` on its boot volume, validates the ELF image, reserves
its physical load range, loads its segments, takes over the firmware's display,
plays the boot chime and hands off to the kernel. QEMU's `-kernel` loader is not
involved in this path.

| File | Role |
| --- | --- |
| `boot/uefi/main.c` | entry point; reads the kernel, `/NXU/bootargs.txt` and the chime; the display |
| `boot/uefi/x86.c`, `transition_x86.S` | x64 firmware: ELF32/ELF64 loading and the two x86 handoffs |
| `boot/uefi/arm64.c`, `enter_arm64.S` | AArch64 firmware: ELF64 loading and the Device Tree handoff |
| `boot/uefi/fdt.c` | reads the firmware's Device Tree and writes the kernel's copy |
| `boot/uefi/chime.c` | the boot chime on the VirtIO Sound device |
| `boot/uefi/virtio_pci.c`, `virtio_mmio.c` | the sound device's transport on x64 and AArch64 |
| `boot/uefi/efi.h` | the slice of the UEFI ABI the loader uses |
| `boot/handoff.h` | the x86 handoff layouts shared with the kernels |

## x86 handoff

The i386 kernel is entered in 32-bit protected mode with paging disabled. The
loader provides a Multiboot 1 memory map and framebuffer metadata, plus a
versioned NXU extension carrying the firmware RSDP. It prefers the ACPI 2.0
configuration-table entry. It obtains a fresh UEFI memory-map key immediately
before ExitBootServices, retries a stale key, then uses a reserved
position-independent transition stub. The x86-64 kernel is entered in long mode
with `struct nxu_boot_x86_64`. Firmware runtime services are not used after the
handoff.

GOP RGB/BGR 32-bit linear framebuffers are supported. When no VirtIO display
binds, the i386 kernel maps this framebuffer and presents from a software surface,
respecting its row pitch and channel order. Graphics acceleration is not supplied.

## arm64 handoff

The arm64 kernel is entered exactly as a direct boot enters it: at its physical
entry point with the MMU and both caches off (`SCTLR` M, C and I clear) and the
Device Tree address in `x0`. The kernel needs no change to tell the two apart.

The ELF64 image is linked at its permanent higher-half addresses; the loader
places each segment at the physical address its program header names
(`0x40080000` on QEMU `virt`) and enters through the physical alias of the entry
point. Before turning the MMU off it writes the data cache back for the kernel
image and the Device Tree.

The Device Tree is the whole handoff. The firmware publishes its tree as a
configuration table (only when the machine runs with ACPI off); the loader gives
the kernel a copy in which

- `/chosen/bootargs` is the line from `/NXU/bootargs.txt`, and
- the memory reservation block lists the memory the boot chime is playing from.

The kernel's physical memory manager keeps every range in the reservation block
out of the allocator, and places its own bitmap clear of them.

## The boot chime

The loader plays the chime, as a machine's firmware does, rather than the kernel.
The firmware has no sound driver, so `chime.c` drives the VirtIO Sound device
itself (VirtIO-PCI on x64, VirtIO-MMIO on AArch64): it converts `/NXU/chime.wav`
to 16-bit stereo, queues all of it on the device's transmit queue, starts the
stream and moves on. The device plays from the queued memory on its own while the
kernel is loaded and starts; the loader never waits for the sound.

The chime marks the display coming up, and is only played with one:

1. The loader looks for the firmware's graphics output. A firmware that booted
   straight from its boot order has connected only the boot device, so the loader
   has it bind its drivers to every controller and looks again.
2. With a display, the loader clears it to black and plays the chime. Without
   one it plays nothing and says nothing to the kernel, which then plays the
   chime itself when its own display output starts (`kern/boot/boot_chime.c`).

What the kernel is told when the loader played the chime:

- `nbl.chime=<milliseconds>`, the chime's length, as the first boot argument.
  The kernel plays no chime of its own. Its sound driver does not reset the device
  until that time (plus a margin for the host's buffer) has passed: a kernel thread
  sleeps it out and attaches then, so the boot is not held up and `/dev/audio0`
  appears a few seconds in. A reset any sooner would cut the sound and strand the
  completions the device still owes the loader's queue.
- The memory holding the queues and the samples is one allocation that stays
  reserved: the x86 memory maps mark loader data reserved, and the arm64 Device
  Tree lists it in its reservation block.

The device is told not to interrupt, so it raises nothing on a line the kernel has
no handler on yet. On takeover the kernel's driver stops and releases the stream
the loader started.

`-no-chime` or `-no-sound` in the boot arguments keeps the loader quiet as well
as the kernel. A boot volume built with `NBL_CHIME=` carries no chime, and the
kernel plays it.

## Build on macOS

Clang, LLVM's `lld-link`/`ld.lld`, Python 3 and QEMU are required. The image builder
(`tools/make_uefi_image.py`) uses Python's standard library; it needs neither GNU
filesystem utilities nor `hdiutil`. Building NXU's separate ext4 system volume
still uses e2fsprogs.

```sh
make nbl                # BUILD/nbl/BOOTX64.EFI
make nbl-arm64          # BUILD/nbl/BOOTAA64.EFI
make nbl-image BUILD_ROOT=BUILD/BUILD-i386-desktop UISERVICE=0 WINDOWSERVER=0
make nbl-image-x86_64
make nbl-image-arm64    # BUILD/<config>/nxu-arm64-boot.img
```

Each image is a 65 MiB GPT/FAT32 boot volume holding the loader at
`/EFI/BOOT/BOOTX64.EFI` or `/EFI/BOOT/BOOTAA64.EFI`, and `/NXU/kernel.elf`,
`/NXU/bootargs.txt` and `/NXU/chime.wav`. It is separate from the ext4 system
volume. `NBL_ARGS` (x86) and `NBL_ARM64_ARGS` override the command line embedded
in the image; `NBL_CHIME` names the WAV file. The arm64 volume carries the kernel
without its debug sections.

## Run through firmware

```sh
make run                  # arm64 desktop
make run-arm64-firmware   # arm64 console boot
make run-i386-desktop
make run-i386-recovery
make run-i386-firmware BUILD_ROOT=BUILD/BUILD-i386-desktop UISERVICE=0 WINDOWSERVER=0
make run-x86_64
```

These boot NBL through UEFI by default. The direct paths stay available:
`make run ARM64_BOOT=direct` and `I386_BOOT=direct` use QEMU's `-kernel` loader,
and `make test TEST=<id>`, `make check` and `make run-console` always do
(`make test TEST=<id> ARM64_BOOT=firmware` boots any test kernel through NBL).

On arm64 the machine runs with `acpi=off` so the firmware hands over a Device
Tree, and the boot volume is a USB stick, which the kernel's VirtIO-MMIO scan
never sees. `NBL_ARM64_FIRMWARE` and `NBL_ARM64_VARS` override the firmware code
and variable templates QEMU supplies (`edk2-aarch64-code.fd`, `edk2-arm-vars.fd`).

On x86, Intel macOS hosts use HVF; other hosts use TCG. The CPU's advertised
physical-address width is limited to 32 bits, and PCI's 64-bit aperture is
disabled, so firmware places device BARs where the current non-PAE kernel can map
them. `run-i386-firmware` is a lean recovery boot without the desktop Rust
frameworks: a separate ext4 system volume, GOP graphics, VirtIO storage, input and
sound. `NBL_FIRMWARE` and `NBL_VARS` override the paired UEFI code and variable
templates. Each run uses a local copy of the variable template.

## Real hardware scope

The image is an unsigned UEFI boot prototype, not a laptop installer. Firmware
must permit its EFI application to execute. The kernel's fixed physical load
range must be available. NXU still needs drivers for the laptop's actual storage,
keyboard/trackpad and other devices to run its installed desktop, and the chime
needs a VirtIO Sound device. The boot volume's firmware filesystem service stops
being available after ExitBootServices.

The loader, the firmware framebuffer path and the chime are tested in QEMU (x64
firmware including macOS HVF, AArch64 firmware under TCG); they have not been
tested on the Dell laptop. No host disks or firmware boot entries are modified by
the build or run targets.

The i386 kernel presents the shared NXU boot splash as soon as VirtIO-GPU attaches,
or after registering the UEFI framebuffer fallback. The logo, progress bar and
boot log stay visible during startup and recovery-key selection. The splash
unregisters its console sink before recovery or the desktop takes over scanout.
