# VirtIO

## Overview

VirtIO is the first reusable device transport in NXU. On the QEMU AArch64
`virt` machine the transport is MMIO and is described by `virtio,mmio` Device
Tree nodes.

The driver stack is divided into four ownership domains:

```text
platform
   |
   | MMIO region + INTID + trigger flags
   v
VirtIO-MMIO transport
   |
   | device identity + features + queue programming
   v
virtqueue
   |
   | descriptor/ring ownership
   v
device driver
```

The platform parser does not infer device class from an MMIO address. It
records transport resources only. The VirtIO bus reads the device ID and binds
a supported class driver.

This boundary is required for upcoming VirtIO Block and VirtIO GPU support:
those drivers reuse the transport and queue code without carrying input-driver
policy into the bus layer.

## Device discovery

`platform_discover()` records every VirtIO-MMIO region described by the DTB,
including its architectural GIC INTID and interrupt trigger flags.

`virtio_init()` walks those transports. For each live device it:

1. validates the VirtIO magic and modern MMIO version;
2. reads the device and vendor IDs;
3. selects a supported class driver;
4. leaves unsupported device IDs untouched.

Empty transport frames are normal on QEMU `virt` and are ignored.

The class drivers are block (`virtio_block.c`), GPU, input (keyboard, mouse) and
sound; the sound driver, which also runs on VirtIO-PCI, has its own page:
[VirtIO Sound](virtio-sound.md). A transport reports through
`virtio_device_t.irq_bound` whether `irq_attach` really put the handler on the
device's interrupt or the transport polls (VirtIO-PCI on x86 polls unless
`virtio-irq=1` is given), which a driver that wants to sleep until the device
interrupts needs to know.

## Transport state

NXU currently accepts modern VirtIO-MMIO version 2 and requires
`VIRTIO_F_VERSION_1`.

Initialization follows this state sequence:

```text
status = 0
   |
ACKNOWLEDGE
   |
DRIVER
   |
feature negotiation
   |
FEATURES_OK
   |
queue configuration
   |
DRIVER_OK
```

If the device rejects negotiated features or queue setup fails, the driver
marks the transport `FAILED` where possible and does not continue with a
partially initialized device.

## MMIO registers

The transport currently uses:

| Offset | Register |
| ---: | --- |
| `0x000` | MagicValue |
| `0x004` | Version |
| `0x008` | DeviceID |
| `0x00C` | VendorID |
| `0x010/0x014` | DeviceFeatures / selector |
| `0x020/0x024` | DriverFeatures / selector |
| `0x030` | QueueSel |
| `0x034` | QueueNumMax |
| `0x038` | QueueNum |
| `0x044` | QueueReady |
| `0x050` | QueueNotify |
| `0x060` | InterruptStatus |
| `0x064` | InterruptACK |
| `0x070` | Status |
| `0x080-0x0A4` | split-ring physical addresses |
| `0x100+` | device-specific configuration |

## Split virtqueues

`virtqueue_t` owns one physical page containing the queue metadata:

```text
+--------------------+
| descriptor table   |
+--------------------+
| available ring     |
+--------------------+
| alignment          |
+--------------------+
| used ring          |
+--------------------+
```

For the current limit of 128 descriptors, the complete split-ring metadata
fits in one 4 KiB page.

The CPU accesses this page through the higher-half direct map. The device sees
physical addresses programmed into the MMIO transport.

### Ownership

The queue owns:

- descriptor allocation state;
- available-ring publication state;
- used-ring consumption state;
- queue metadata backing memory.

The device driver owns:

- request/event buffers;
- descriptor contents;
- descriptor-chain semantics;
- notification policy.

A queue must not be destroyed while the device can still reference its rings.

## Memory ordering

Queue publication and completion use AArch64 DMA barriers. Descriptor and
buffer stores are ordered before the available index becomes visible to the
device; used-ring reads are ordered before completed data is consumed.

The current QEMU `virt` target is treated as DMA coherent, so the driver does
not issue explicit cache-line clean/invalidate operations around VirtIO queue
buffers.

An IOMMU or non-coherent DMA target will require a separate DMA mapping layer.

## Interrupt handling

The transport interrupt is routed through the generic IRQ layer:

```text
GIC
 |
 v
irq_dispatch(INTID)
 |
 v
VirtIO device interrupt routine
 |-- read InterruptStatus
 |-- write InterruptACK
 `-- consume used-ring entries
```

The device routine acknowledges VirtIO interrupt status. Exception entry owns
GIC acknowledge and EOI.

QEMU's device tree calls the VirtIO-MMIO interrupts edge-triggered, and the GIC
is set up that way, but the device holds its line up until InterruptStatus is
acknowledged. A status raised before the handler was bound (the queues are live
first) rises while the SPI is still off, so its edge is lost and, unacknowledged,
the line never falls to rise again: the device never interrupts, and everything
waiting on it falls back to polling. `irq_attach` therefore acknowledges any
status already pending once the SPI is on. This is what kept the mouse polled at
the desktop's 10 ms tick (about 50 frames a second) until 2026-09-27.

## Locking

Virtqueue operations currently rely on the owning driver to serialize queue
access. The input driver submits and consumes its event queue from controlled
initialization/IRQ paths, so no general queue lock is required yet.

A future block/GPU driver with concurrent submitters must introduce an
explicit queue lock or a higher-level serialized request path.

## Limitations

The current transport intentionally supports only the subset required by the
QEMU `virt` target:

- modern VirtIO-MMIO;
- split rings;
- no packed rings;
- no indirect descriptors;
- no `EVENT_IDX`;
- coherent DMA assumption;
- no IOMMU mappings;
- no hot removal.

Unsupported functionality is rejected or left unnegotiated rather than
silently emulated.
