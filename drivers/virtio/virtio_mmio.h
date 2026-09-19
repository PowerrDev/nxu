#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_MMIO_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_MMIO_H

#include <drivers/virtio/virtio_transport.h>
#include <drivers/virtio/virtqueue.h>
#include <platform/platform.h>

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_MMIO_MAGIC_VALUE 0x74726976U
#define VIRTIO_MMIO_VERSION_MODERN 2U


/*
 * The MMIO transport is one implementation of the transport interface in
 * virtio_transport.h; the device object it fills in is the common
 * virtio_device_t, with ops pointing at virtio_mmio_ops.
 */
typedef virtio_device_t virtio_mmio_device_t;

extern const virtio_transport_ops_t virtio_mmio_ops;

/*
 * virtio_mmio_probe:
 *
 * Inspect one MMIO transport described by the platform layer.
 *
 * Probe is read-only with respect to device status. Empty transports are
 * expected on the QEMU virt machine and return false without side effects.
 * The returned object records transport identity and interrupt routing only;
 * feature negotiation is performed separately by virtio_mmio_begin().
 *
 * Returns true for a live modern VirtIO-MMIO device.
 */
bool virtio_mmio_probe(
	const platform_region_t *region,
	uint32_t intid,
	uint32_t irq_flags,
	virtio_mmio_device_t *device
);

/*
 * virtio_mmio_begin:
 *
 * Reset a probed device and perform transport-level feature negotiation.
 * VIRTIO_F_VERSION_1 is mandatory. Device-specific accepted features are
 * supplied by the class driver.
 *
 * Failure leaves the device marked FAILED where possible.
 *
 * Returns true after FEATURES_OK has been accepted by the device.
 */
bool virtio_mmio_begin(
	virtio_mmio_device_t *device,
	uint64_t accepted_device_features
);

/*
 * virtio_mmio_setup_queue:
 *
 * Publish one initialized split virtqueue to a negotiated device queue.
 * Queue ownership remains with the caller; the transport only programs the
 * queue size and physical descriptor/available/used addresses.
 *
 * Returns true when the device accepted the queue configuration.
 */
bool virtio_mmio_setup_queue(
	virtio_mmio_device_t *device,
	uint16_t queue_index,
	virtqueue_t *queue
);

/*
 * virtio_mmio_finish:
 *
 * Publish DRIVER_OK after all device queues and class state are ready.
 *
 * Returns true when the final device status was accepted.
 */
bool virtio_mmio_finish(virtio_mmio_device_t *device);

/*
 * virtio_mmio_reset:
 *
 * Reset a device and wait for transport status to return to zero. Queue and
 * DMA memory may be reclaimed after this operation completes.
 */
bool virtio_mmio_reset(
	virtio_mmio_device_t *device
);

void virtio_mmio_fail(virtio_mmio_device_t *device);
void virtio_mmio_notify(virtio_mmio_device_t *device, uint16_t queue_index);
uint32_t virtio_mmio_interrupt_status(const virtio_mmio_device_t *device);
void virtio_mmio_interrupt_ack(virtio_mmio_device_t *device, uint32_t status);

uint8_t virtio_mmio_config_read8(
	const virtio_mmio_device_t *device,
	uint32_t offset
);

uint32_t virtio_mmio_config_read32(
	const virtio_mmio_device_t *device,
	uint32_t offset
);

uint64_t virtio_mmio_config_read64(
	const virtio_mmio_device_t *device,
	uint32_t offset
);

void virtio_mmio_config_write8(
	virtio_mmio_device_t *device,
	uint32_t offset,
	uint8_t value
);

#endif
