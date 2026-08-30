# Block Storage

## Overview

The NXU block layer presents sector-addressed storage independently of the
transport that supplies it. Filesystems and the future VFS operate on
`block_device_t`; they do not submit VirtIO descriptors directly.

The first transport implementation is VirtIO Block over the existing modern
VirtIO-MMIO core.

## Block device contract

A registered block device publishes:

- a stable device identifier and name;
- total protocol-sector count;
- protocol sector size;
- advertised logical block size;
- read-only state;
- synchronous read, write and flush operations;
- driver-private state owned by the class driver.

All NXU block I/O is currently expressed in 512-byte sectors. Range checking
is performed by the block layer before the request enters the device driver.

## VirtIO Block

VirtIO Block devices use device ID 2. NXU negotiates the modern transport and
supports the read-only, logical-block-size and flush feature bits.

Only request queue 0 is used. Multi-queue operation is not negotiated.

Each normal request uses three split-ring descriptors:

```text
header -> data -> status
````

The header and status byte live in one persistent DMA control page. A second
4 KiB DMA page is used as a bounce buffer for request data. Reads mark the data
descriptor device-writable; writes leave it device-readable.

The bounce page limits one submitted request to eight 512-byte sectors. Larger
block-layer operations are split transparently into multiple requests.

## Completion model

The initial driver is synchronous and polled. Request-queue interrupts are
suppressed and the submitting thread consumes the used ring until ownership of
its descriptor head returns from the device.

This is an intentional bootstrap design. It keeps filesystem code independent
of scheduler wait queues while preserving the same block-device interface that
an asynchronous driver can implement later.

The request queue and shared bounce page are serialized by a driver lock.

## Flush semantics

If flush support is negotiated, `block_device_flush()` submits a flush request
with no data descriptor.

If the device does not advertise flush, the block layer treats the operation as
already complete.

Read-only devices reject writes before they enter the transport driver.

## DMA ownership

Virtqueue metadata, request headers, status bytes and bounce buffers are
allocated from physical pages and accessed through the permanent higher-half
direct map.

Device descriptors always contain physical addresses.

A descriptor is not returned to the free list until its used-ring completion
has been observed.

## Boot validation

`kern_init` performs one read-only sector-zero request after VirtIO discovery.

The first sixteen bytes are printed to UART.

The test never writes the medium, so the same development image can later be
formatted as ext4.
