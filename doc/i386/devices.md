# i386 devices

Platform discovery, PCI, VirtIO over PCI, and the block and input drivers on
`qemu-system-i386 -M pc`. Owned by the devices area; see `port.md` for the
port as a whole.

## What runs

* `i386_init_platform()` (`platform/i386/devices_init.c`): `platform_bootstrap()`
  fills `platform_t` (COM1 and CMOS I/O port ranges, the Multiboot memory map
  through the VM area's `i386_memory_map_load()`, PCI counts), dumps it and the
  PCI table to the boot log, and brings up the CMOS RTC.
* `i386_init_drivers()`: input and block cores, then `virtio_init()`, which
  scans the (empty) MMIO list and then the VirtIO-PCI scanner registered with
  `virtio_bus_register()`.
* `test=platform` and `test=drivers` run the self-tests (below).

## Files

| File | Role |
|------|------|
| `platform/i386/pci.{h,c}` | config mechanism 1 (0xCF8/0xCFC), bus/bridge/multifunction scan, BAR sizing, capability walk, `pci_*` API |
| `platform/i386/platform.c`, `rtc.c` | `platform.h` and `rtc.h` for the PC |
| `platform/i386/devices_init.c` | the two boot phases and self-tests |
| `drivers/virtio/virtio_transport.h` | `virtio_device_t` and the transport ops table |
| `drivers/virtio/virtio_mmio.c` | MMIO transport (arm64), now `virtio_mmio_ops` |
| `drivers/virtio/virtio_pci.{h,c}` | legacy and modern PCI transports, PCI bus scanner |
| `kern/machine/barrier.h` | `ml_dma_wmb/rmb/mb`, `ml_cpu_relax` |
| `makedefs/i386/devices.mk`, `tools/test_i386_devices.sh` | build fragment and test |

## Transports

The block, input and GPU drivers call `virtio_device_*` wrappers that dispatch
through `virtio_transport_ops_t`; arm64 uses `virtio_mmio_ops`, the PC uses:

* **legacy VirtIO-PCI** (I/O-port BAR0, device IDs 0x1000-0x103F). Used for
  virtio-blk. The device fixes the queue size, so `virtqueue_init_legacy()`
  lays the rings out for it (used ring on the next 4 KiB boundary, several
  contiguous pages).
* **modern VirtIO-PCI** (vendor capabilities in memory BARs, IDs 0x1040+).
  **QEMU's virtio-input-pci has no legacy interface** (`1af4:1052`, memory BARs
  only), so the keyboard and mouse can only be driven this way. Reaching a
  memory BAR needs `pci_map_bar()`: with paging off the physical address is
  used directly; with paging on it calls `void *i386_mmio_map(uint64_t
  physical, uint64_t size)`, a weak hook the VM area must supply. Without it
  the input devices are skipped with a message and block still works.

## Interrupts

Polling is the default (the block driver spins on the used ring; call
`virtio_input_service()` periodically for input). Boot arg `virtio-irq=1`
(or `virtio_pci_set_irq_mode(true)`) selects interrupts: each driver chains
its handler onto the device's PCI interrupt line (PIC IRQ, config offset
0x3C) with `irq_register()`, then `pic_set_level_triggered(line, true)` and
`pic_unmask(line)`. Shared INTx lines work because each handler reads its own
ISR byte. A refused registration falls back to polling. The default in
`i386_init_drivers()` is polling; the integrator flips it by passing
`virtio-irq=1` or calling `virtio_pci_set_irq_mode(true)` before `virtio_init()`.

## Standalone stand-ins

Weak definitions, replaced automatically when the real provider is linked:

* `platform/i386/devices_shim.c`: `pmm_allocate_page/free_page`,
  `pmm_allocate_contiguous_pages/free_contiguous_pages`,
  `vmm_higher_half_direct_map_enabled`, `vmm_physical_to_higher_half` (a 256 KiB
  page pool in bss, correct with paging off or on) and `irq_register` /
  `irq_unregister` (refuse, so drivers poll). Drop the file from
  `makedefs/i386/devices.mk` once vm/pmm.c, vm/vmm.c and kern/irq/irq.c are built.
* `platform/i386/virtio_stubs.c`: `virtio_mmio_probe` (no MMIO devices on the PC)
  and the virtio-gpu entry points (display is out of scope).
* `i386_memory_map_load` is a weak reference in `platform.c`; null means "map
  unavailable".

## Boot arguments (self-tests)

`expect-virtio=<n>`, `expect-time=<unix>`, `expect-block=<n>` (default 1),
`expect-input=<n>`, `input-wait=<sec>`, `block-keep=1`, `virtio-irq=1`.

## QEMU devices and test

```
qemu-system-i386 -M pc -kernel kernel.elf -m 512M -display none -serial stdio \
  -monitor none -no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
  -drive if=none,format=raw,file=<scratch.img>,id=d0 \
  -device virtio-blk-pci,drive=d0,disable-modern=on \
  -device virtio-keyboard-pci,disable-modern=on \
  -device virtio-mouse-pci,disable-modern=on \
  -append "test=drivers qemu-exit=1"
```

`disable-modern=on` makes virtio-blk legacy (I/O BAR0); it has no effect on the
input devices, which stay modern-only. `make test-i386-devices` (or
`tools/test_i386_devices.sh <kernel.elf>`) builds scratch disks with `dd` and
covers: PCI and RTC (against the host clock), block read/write/flush with the
result checked on the host image, polling and interrupt delivery, a missing
disk, and key/mouse events injected through the QEMU monitor.

## Known gaps

No MSI/MSI-X, no PCI-to-PCI bridge resource assignment (firmware's BAR
assignments are used), no memory-BAR mapping under paging until the VM area
provides `i386_mmio_map`, virtio-gpu/ramfb/fw_cfg not built, one block request
in flight at a time (existing driver design).
