#include <drivers/virtio/virtio_mmio.h>

#include <kern/irq/irq.h>
#include <kern/arm64/gic.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VIRTIO_MMIO_MAGIC_VALUE_OFFSET 0x000U
#define VIRTIO_MMIO_VERSION_OFFSET 0x004U
#define VIRTIO_MMIO_DEVICE_ID_OFFSET 0x008U
#define VIRTIO_MMIO_VENDOR_ID_OFFSET 0x00CU

#define VIRTIO_MMIO_DEVICE_FEATURES_OFFSET 0x010U
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET 0x014U

#define VIRTIO_MMIO_DRIVER_FEATURES_OFFSET 0x020U
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET 0x024U

#define VIRTIO_MMIO_QUEUE_SEL_OFFSET 0x030U
#define VIRTIO_MMIO_QUEUE_NUM_MAX_OFFSET 0x034U
#define VIRTIO_MMIO_QUEUE_NUM_OFFSET 0x038U
#define VIRTIO_MMIO_QUEUE_READY_OFFSET 0x044U
#define VIRTIO_MMIO_QUEUE_NOTIFY_OFFSET 0x050U

#define VIRTIO_MMIO_INTERRUPT_STATUS_OFFSET 0x060U
#define VIRTIO_MMIO_INTERRUPT_ACK_OFFSET 0x064U

#define VIRTIO_MMIO_STATUS_OFFSET 0x070U

#define VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET 0x080U
#define VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET 0x084U
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW_OFFSET 0x090U
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH_OFFSET 0x094U
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW_OFFSET 0x0A0U
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH_OFFSET 0x0A4U

#define VIRTIO_MMIO_CONFIG_GENERATION_OFFSET 0x0FCU
#define VIRTIO_MMIO_CONFIG_OFFSET 0x100U

#define VIRTIO_MMIO_GIC_PRIORITY 0x90U
#define VIRTIO_MMIO_IRQ_TYPE_EDGE_RISING 0x01U
#define VIRTIO_MMIO_IRQ_TYPE_EDGE_FALLING 0x02U

/*
 * virtio_mmio_read32:
 *
 * Read one 32-bit transport register.
 *
 * MMIO accesses are volatile and must not be folded, cached or reordered
 * into ordinary memory accesses by the compiler.
 */
static inline uint32_t virtio_mmio_read32(
	const virtio_mmio_device_t *device,
	uint32_t offset
)
{
	return *(volatile uint32_t *)(device->mmio_base + offset);
}

/*
 * virtio_mmio_write32:
 *
 * Write one 32-bit transport register.
 */
static inline void virtio_mmio_write32(
	virtio_mmio_device_t *device,
	uint32_t offset,
	uint32_t value
)
{
	*(volatile uint32_t *)(device->mmio_base + offset) = value;
}

/*
 * virtio_mmio_barrier:
 *
 * Order transport MMIO and DMA-visible memory accesses against the device.
 */
static inline void virtio_mmio_barrier(void)
{
	__asm__ volatile("dmb osh" : : : "memory");
}

/*
 * virtio_mmio_read_features:
 *
 * Read both 32-bit device feature banks and combine them into one 64-bit
 * feature mask.
 */
static uint64_t virtio_mmio_read_features(virtio_mmio_device_t *device)
{
	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET,
		0U
	);

	uint64_t low = virtio_mmio_read32(
		device,
		VIRTIO_MMIO_DEVICE_FEATURES_OFFSET
	);

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET,
		1U
	);

	uint64_t high = virtio_mmio_read32(
		device,
		VIRTIO_MMIO_DEVICE_FEATURES_OFFSET
	);

	return low | (high << 32U);
}

/*
 * virtio_mmio_write_features:
 *
 * Publish the negotiated driver feature mask through the two modern MMIO
 * feature banks.
 */
static void virtio_mmio_write_features(
	virtio_mmio_device_t *device,
	uint64_t features
)
{
	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET,
		0U
	);

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_DRIVER_FEATURES_OFFSET,
		(uint32_t)features
	);

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET,
		1U
	);

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_DRIVER_FEATURES_OFFSET,
		(uint32_t)(features >> 32U)
	);
}

/*
 * virtio_mmio_write_address:
 *
 * Publish one 64-bit queue physical address through the paired low/high
 * transport registers.
 */
static void virtio_mmio_write_address(
	virtio_mmio_device_t *device,
	uint32_t low_offset,
	uint32_t high_offset,
	uint64_t address
)
{
	virtio_mmio_write32(
		device,
		low_offset,
		(uint32_t)address
	);

	virtio_mmio_write32(
		device,
		high_offset,
		(uint32_t)(address >> 32U)
	);
}

/*
 * virtio_mmio_probe:
 *
 * Inspect one platform-provided VirtIO-MMIO frame without changing device
 * ownership or transport status.
 *
 * Empty MMIO frames and legacy transports are ignored.
 */
bool virtio_mmio_probe(
	const platform_region_t *region,
	uint32_t intid,
	uint32_t irq_flags,
	virtio_mmio_device_t *device
)
{
	if (region == 0 || device == 0 || region->size < 0x200ULL) return false;

	if (!vmm_higher_half_direct_map_enabled()) return false;

	uint64_t virtual_address;

	if (!vmm_physical_to_higher_half(
		region->base,
		&virtual_address
	)) {
		return false;
	}

	virtio_mmio_device_t probe;

	memset(
		&probe,
		0,
		sizeof(probe)
	);

	probe.ops = &virtio_mmio_ops;
	probe.region = *region;
	probe.mmio_base = virtual_address;
	probe.intid = intid;
	probe.irq_flags = irq_flags;

	uint32_t magic = virtio_mmio_read32(
		&probe,
		VIRTIO_MMIO_MAGIC_VALUE_OFFSET
	);

	if (magic != VIRTIO_MMIO_MAGIC_VALUE) return false;

	uint32_t version = virtio_mmio_read32(
		&probe,
		VIRTIO_MMIO_VERSION_OFFSET
	);

	if (version != VIRTIO_MMIO_VERSION_MODERN) return false;

	probe.device_id = virtio_mmio_read32(
		&probe,
		VIRTIO_MMIO_DEVICE_ID_OFFSET
	);

	if (probe.device_id == VIRTIO_DEVICE_ID_NONE) return false;

	probe.vendor_id = virtio_mmio_read32(
		&probe,
		VIRTIO_MMIO_VENDOR_ID_OFFSET
	);

	*device = probe;

	return true;
}

/*
 * virtio_mmio_reset:
 *
 * Reset a transport and wait until the device acknowledges status zero.
 *
 * Queue and DMA storage associated with the device may be reclaimed only
 * after reset has completed.
 */
bool virtio_mmio_reset(virtio_mmio_device_t *device)
{
	if (device == 0 || device->mmio_base == 0ULL) return false;

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_STATUS_OFFSET,
		0U
	);

	virtio_mmio_barrier();

	while (
		virtio_mmio_read32(
			device,
			VIRTIO_MMIO_STATUS_OFFSET
		) != 0U
	) {
		__asm__ volatile("yield");
	}

	device->device_features = 0ULL;
	device->driver_features = 0ULL;

	device->initialized = false;
	device->driver_ok = false;

	return true;
}

/*
 * virtio_mmio_fail:
 *
 * Mark the transport failed when initialization cannot continue safely.
 */
void virtio_mmio_fail(virtio_mmio_device_t *device)
{
	if (device == 0 || device->mmio_base == 0ULL) return;

	uint32_t status = virtio_mmio_read32(
		device,
		VIRTIO_MMIO_STATUS_OFFSET
	);

	status |= VIRTIO_STATUS_FAILED;

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_STATUS_OFFSET,
		status
	);

	virtio_mmio_barrier();
}

/*
 * virtio_mmio_begin:
 *
 * Reset a live device and complete modern feature negotiation through
 * FEATURES_OK.
 *
 * accepted_device_features contains only device-specific features understood
 * by the class driver. VIRTIO_F_VERSION_1 is always required by the transport
 * layer.
 */
bool virtio_mmio_begin(
	virtio_mmio_device_t *device,
	uint64_t accepted_device_features
)
{
	if (device == 0 || device->device_id == VIRTIO_DEVICE_ID_NONE) return false;

	if (device->initialized) return false;

	if (!virtio_mmio_reset(device)) return false;

	uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE;

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_STATUS_OFFSET,
		status
	);

	status |= VIRTIO_STATUS_DRIVER;

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_STATUS_OFFSET,
		status
	);

	device->device_features = virtio_mmio_read_features(device);

	uint64_t version_feature = 1ULL << VIRTIO_F_VERSION_1;

	if (
		(device->device_features & version_feature) ==
		0ULL
	) {
		virtio_mmio_fail(device);
		return false;
	}

	uint64_t supported =
		accepted_device_features |
		version_feature;

	device->driver_features =
		device->device_features &
		supported;

	device->driver_features |= version_feature;

	virtio_mmio_write_features(
		device,
		device->driver_features
	);

	status |= VIRTIO_STATUS_FEATURES_OK;

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_STATUS_OFFSET,
		status
	);

	virtio_mmio_barrier();

	uint32_t accepted_status =
		virtio_mmio_read32(
			device,
			VIRTIO_MMIO_STATUS_OFFSET
		);

	if (
		(accepted_status & VIRTIO_STATUS_FEATURES_OK) ==
		0U
	) {
		virtio_mmio_fail(device);
		return false;
	}

	device->initialized = true;

	return true;
}

/*
 * virtio_mmio_setup_queue:
 *
 * Publish split-ring size and physical addresses for one queue negotiated by
 * the class driver.
 */
bool virtio_mmio_setup_queue(
	virtio_mmio_device_t *device,
	uint16_t queue_index,
	virtqueue_t *queue
)
{
	if (
		device == 0 ||
		queue == 0 ||
		!device->initialized ||
		!queue->initialized
	) {
		return false;
	}

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_QUEUE_SEL_OFFSET,
		queue_index
	);

	if (
		virtio_mmio_read32(
			device,
			VIRTIO_MMIO_QUEUE_READY_OFFSET
		) != 0U
	) {
		return false;
	}

	uint32_t maximum = virtio_mmio_read32(
		device,
		VIRTIO_MMIO_QUEUE_NUM_MAX_OFFSET
	);

	if (maximum == 0U || queue->size > maximum) return false;

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_QUEUE_NUM_OFFSET,
		queue->size
	);

	virtio_mmio_write_address(
		device,
		VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET,
		VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET,
		virtqueue_desc_physical(queue)
	);

	virtio_mmio_write_address(
		device,
		VIRTIO_MMIO_QUEUE_DRIVER_LOW_OFFSET,
		VIRTIO_MMIO_QUEUE_DRIVER_HIGH_OFFSET,
		virtqueue_available_physical(queue)
	);

	virtio_mmio_write_address(
		device,
		VIRTIO_MMIO_QUEUE_DEVICE_LOW_OFFSET,
		VIRTIO_MMIO_QUEUE_DEVICE_HIGH_OFFSET,
		virtqueue_used_physical(queue)
	);

	virtio_mmio_barrier();

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_QUEUE_READY_OFFSET,
		1U
	);

	virtio_mmio_barrier();

	return virtio_mmio_read32(
		device,
		VIRTIO_MMIO_QUEUE_READY_OFFSET
	) == 1U;
}

/*
 * virtio_mmio_finish:
 *
 * Transfer final device ownership to the driver by publishing DRIVER_OK.
 *
 * All queues, DMA storage and interrupt state required by the class driver
 * must already be initialized.
 */
bool virtio_mmio_finish(virtio_mmio_device_t *device)
{
	if (
		device == 0 ||
		!device->initialized ||
		device->driver_ok
	) {
		return false;
	}

	uint32_t status = virtio_mmio_read32(
		device,
		VIRTIO_MMIO_STATUS_OFFSET
	);

	status |= VIRTIO_STATUS_DRIVER_OK;

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_STATUS_OFFSET,
		status
	);

	virtio_mmio_barrier();

	uint32_t confirmed = virtio_mmio_read32(
		device,
		VIRTIO_MMIO_STATUS_OFFSET
	);

	if (
		(confirmed & VIRTIO_STATUS_DRIVER_OK) ==
		0U
	) {
		virtio_mmio_fail(device);
		return false;
	}

	device->driver_ok = true;

	return true;
}

/*
 * virtio_mmio_notify:
 *
 * Notify the device that new descriptors have been published to one queue.
 */
void virtio_mmio_notify(
	virtio_mmio_device_t *device,
	uint16_t queue_index
)
{
	if (device == 0 || !device->driver_ok) return;

	virtio_mmio_barrier();

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_QUEUE_NOTIFY_OFFSET,
		queue_index
	);
}

/*
 * virtio_mmio_interrupt_status:
 *
 * Read pending transport interrupt reasons.
 */
uint32_t virtio_mmio_interrupt_status(
	const virtio_mmio_device_t *device
)
{
	if (device == 0 || !device->initialized) return 0U;

	return virtio_mmio_read32(
		device,
		VIRTIO_MMIO_INTERRUPT_STATUS_OFFSET
	);
}

/*
 * virtio_mmio_interrupt_ack:
 *
 * Acknowledge transport interrupt reasons already consumed by the class
 * driver.
 */
void virtio_mmio_interrupt_ack(
	virtio_mmio_device_t *device,
	uint32_t status
)
{
	if (
		device == 0 ||
		!device->initialized ||
		status == 0U
	) {
		return;
	}

	virtio_mmio_write32(
		device,
		VIRTIO_MMIO_INTERRUPT_ACK_OFFSET,
		status
	);

	virtio_mmio_barrier();
}

/*
 * virtio_mmio_config_read8:
 *
 * Read one byte from device-specific configuration space.
 */
uint8_t virtio_mmio_config_read8(
	const virtio_mmio_device_t *device,
	uint32_t offset
)
{
	if (device == 0 || !device->initialized) return 0U;

	return *(volatile uint8_t *)(
		device->mmio_base +
		VIRTIO_MMIO_CONFIG_OFFSET +
		offset
	);
}

/*
 * virtio_mmio_config_read32:
 *
 * Read one little-endian 32-bit device-specific field from a stable
 * ConfigGeneration.
 */
uint32_t virtio_mmio_config_read32(
	const virtio_mmio_device_t *device,
	uint32_t offset
)
{
	if (device == 0 || !device->initialized) return 0U;

	for (;;) {
		uint32_t before = virtio_mmio_read32(
			device,
			VIRTIO_MMIO_CONFIG_GENERATION_OFFSET
		);

		uint32_t value = 0U;

		for (uint32_t byte = 0U; byte < 4U; byte++) {
			value |=
				(uint32_t)virtio_mmio_config_read8(
					device,
					offset + byte
				) << (byte * 8U);
		}

		__asm__ volatile(
			"dmb oshld"
			:
			:
			: "memory"
		);

		uint32_t after = virtio_mmio_read32(
			device,
			VIRTIO_MMIO_CONFIG_GENERATION_OFFSET
		);

		if (before == after) return value;
	}
}

/*
 * virtio_mmio_config_read64:
 *
 * Read one little-endian 64-bit device-specific field from a stable
 * ConfigGeneration.
 */
uint64_t virtio_mmio_config_read64(
	const virtio_mmio_device_t *device,
	uint32_t offset
)
{
	if (device == 0 || !device->initialized) return 0ULL;

	for (;;) {
		uint32_t before = virtio_mmio_read32(
			device,
			VIRTIO_MMIO_CONFIG_GENERATION_OFFSET
		);

		uint64_t value = 0ULL;

		for (uint32_t byte = 0U; byte < 8U; byte++) {
			value |=
				(uint64_t)virtio_mmio_config_read8(
					device,
					offset + byte
				) << (byte * 8U);
		}

		__asm__ volatile(
			"dmb oshld"
			:
			:
			: "memory"
		);

		uint32_t after = virtio_mmio_read32(
			device,
			VIRTIO_MMIO_CONFIG_GENERATION_OFFSET
		);

		if (before == after) return value;
	}
}

/*
 * virtio_mmio_config_write8:
 *
 * Write one byte to device-specific configuration space.
 */
void virtio_mmio_config_write8(
	virtio_mmio_device_t *device,
	uint32_t offset,
	uint8_t value
)
{
	if (device == 0 || !device->initialized) return;

	*(volatile uint8_t *)(
		device->mmio_base +
		VIRTIO_MMIO_CONFIG_OFFSET +
		offset
	) = value;

	virtio_mmio_barrier();
}

/*
 * virtio_mmio_queue_init:
 *
 * MMIO lets the driver choose the queue size (bounded by QueueNumMax when the
 * queue is published), so the requested size is used as given.
 */
static bool virtio_mmio_queue_init(
	virtio_device_t *device,
	uint16_t queue_index,
	uint16_t size,
	virtqueue_t *queue
)
{
	(void)device;
	(void)queue_index;

	return virtqueue_init(queue, size);
}

/*
 * virtio_mmio_irq_attach:
 *
 * Bind the class driver's handler to this transport's GIC INTID and enable
 * the SPI. The registration is undone if the GIC rejects the INTID.
 */
static bool virtio_mmio_irq_attach(
	virtio_device_t *device,
	irq_handler_t handler,
	void *context
)
{
	if (!irq_register(device->intid, handler, context)) return false;

	bool edge_triggered =
		(device->irq_flags &
		(VIRTIO_MMIO_IRQ_TYPE_EDGE_RISING | VIRTIO_MMIO_IRQ_TYPE_EDGE_FALLING)) != 0U;

	if (!gic_enable_spi(device->intid, VIRTIO_MMIO_GIC_PRIORITY, edge_triggered)) {
		(void)irq_unregister(device->intid, handler, context);
		return false;
	}

	return true;
}

const virtio_transport_ops_t virtio_mmio_ops = {
	.name = "virtio-mmio",
	.begin = virtio_mmio_begin,
	.queue_init = virtio_mmio_queue_init,
	.setup_queue = virtio_mmio_setup_queue,
	.finish = virtio_mmio_finish,
	.reset = virtio_mmio_reset,
	.fail = virtio_mmio_fail,
	.notify = virtio_mmio_notify,
	.interrupt_status = virtio_mmio_interrupt_status,
	.interrupt_ack = virtio_mmio_interrupt_ack,
	.irq_attach = virtio_mmio_irq_attach,
	.config_read8 = virtio_mmio_config_read8,
	.config_read32 = virtio_mmio_config_read32,
	.config_read64 = virtio_mmio_config_read64,
	.config_write8 = virtio_mmio_config_write8
};
