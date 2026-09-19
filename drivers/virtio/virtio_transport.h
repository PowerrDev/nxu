#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_TRANSPORT_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_TRANSPORT_H

#include <drivers/virtio/virtqueue.h>
#include <kern/irq/irq.h>
#include <platform/platform.h>

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01U
#define VIRTIO_STATUS_DRIVER 0x02U
#define VIRTIO_STATUS_DRIVER_OK 0x04U
#define VIRTIO_STATUS_FEATURES_OK 0x08U
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 0x40U
#define VIRTIO_STATUS_FAILED 0x80U

#define VIRTIO_F_VERSION_1 32U

#define VIRTIO_DEVICE_ID_NONE 0U
#define VIRTIO_DEVICE_ID_BLOCK 2U
#define VIRTIO_DEVICE_ID_GPU 16U
#define VIRTIO_DEVICE_ID_INPUT 18U

/* Interrupt status bits: identical for the MMIO register and the PCI ISR byte. */
#define VIRTIO_INTERRUPT_USED_BUFFER 0x01U
#define VIRTIO_INTERRUPT_CONFIG 0x02U

/* Queues a modern PCI device may have programmed; blk 1, input 2, gpu 2. */
#define VIRTIO_PCI_MAX_QUEUES 4U

struct virtio_device;

/*
 * struct virtio_transport_ops
 *
 * What a class driver may ask of the bus the device sits on. Every function
 * takes the device the ops table belongs to. The class drivers (block, input,
 * GPU) and the virtqueue code are written against these operations only, so
 * VirtIO-MMIO (arm64 virt), legacy VirtIO-PCI (I/O-port BAR) and modern
 * VirtIO-PCI (capabilities in a memory BAR) all run the same driver code.
 *
 * begin       reset, ACKNOWLEDGE|DRIVER, negotiate features up to and
 *             including FEATURES_OK where the transport has that step
 * queue_init  create the virtqueue this transport needs for queue_index. The
 *             driver states the size it would like; a transport whose device
 *             fixes the size (legacy PCI) uses the device's size instead
 * setup_queue publish an initialized queue to the device
 * finish      DRIVER_OK
 * irq_attach  bind handler(context) to the device's interrupt and unmask it;
 *             true also when the transport polls instead
 */
typedef struct virtio_transport_ops {
	const char *name;

	bool (*begin)(struct virtio_device *device, uint64_t accepted_device_features);
	bool (*queue_init)(struct virtio_device *device, uint16_t queue_index, uint16_t size, virtqueue_t *queue);
	bool (*setup_queue)(struct virtio_device *device, uint16_t queue_index, virtqueue_t *queue);
	bool (*finish)(struct virtio_device *device);
	bool (*reset)(struct virtio_device *device);
	void (*fail)(struct virtio_device *device);
	void (*notify)(struct virtio_device *device, uint16_t queue_index);
	uint32_t (*interrupt_status)(const struct virtio_device *device);
	void (*interrupt_ack)(struct virtio_device *device, uint32_t status);
	bool (*irq_attach)(struct virtio_device *device, irq_handler_t handler, void *context);

	uint8_t (*config_read8)(const struct virtio_device *device, uint32_t offset);
	uint32_t (*config_read32)(const struct virtio_device *device, uint32_t offset);
	uint64_t (*config_read64)(const struct virtio_device *device, uint32_t offset);
	void (*config_write8)(struct virtio_device *device, uint32_t offset, uint8_t value);
} virtio_transport_ops_t;

/*
 * State for a VirtIO device reached over PCI. Legacy devices are driven
 * through the I/O-port BAR; modern ones through the windows the vendor
 * capabilities describe, already mapped by the transport.
 */
typedef struct {
	uint8_t bus;
	uint8_t slot;
	uint8_t function;
	uint8_t interrupt_line;

	uint16_t io_base;

	volatile uint8_t *common;
	volatile uint8_t *notify;
	volatile uint8_t *isr;
	volatile uint8_t *device_config;
	uint32_t notify_multiplier;
	uint16_t queue_notify_offset[VIRTIO_PCI_MAX_QUEUES];
} virtio_pci_state_t;

/*
 * struct virtio_device
 *
 * One VirtIO device, whatever bus it was found on. region, intid and
 * irq_flags describe where the transport lives and how its interrupt is
 * routed: an MMIO frame and GIC INTID on arm64, the I/O base or BAR address
 * and the PIC line (PCI interrupt line register) on x86.
 */
typedef struct virtio_device {
	const virtio_transport_ops_t *ops;
	platform_region_t region;
	uint64_t mmio_base;
	uint32_t intid;
	uint32_t irq_flags;
	uint32_t device_id;
	uint32_t vendor_id;
	uint64_t device_features;
	uint64_t driver_features;
	bool initialized;
	bool driver_ok;
	virtio_pci_state_t pci;
} virtio_device_t;

static inline bool virtio_device_live(const virtio_device_t *device)
{
	return device != 0 && device->ops != 0;
}

static inline bool virtio_device_begin(virtio_device_t *device, uint64_t accepted_device_features)
{
	return virtio_device_live(device) && device->ops->begin(device, accepted_device_features);
}

static inline bool virtio_device_queue_init(virtio_device_t *device, uint16_t queue_index, uint16_t size, virtqueue_t *queue)
{
	return virtio_device_live(device) && device->ops->queue_init(device, queue_index, size, queue);
}

static inline bool virtio_device_setup_queue(virtio_device_t *device, uint16_t queue_index, virtqueue_t *queue)
{
	return virtio_device_live(device) && device->ops->setup_queue(device, queue_index, queue);
}

static inline bool virtio_device_finish(virtio_device_t *device)
{
	return virtio_device_live(device) && device->ops->finish(device);
}

static inline bool virtio_device_reset(virtio_device_t *device)
{
	return virtio_device_live(device) && device->ops->reset(device);
}

static inline void virtio_device_fail(virtio_device_t *device)
{
	if (virtio_device_live(device)) device->ops->fail(device);
}

static inline void virtio_device_notify(virtio_device_t *device, uint16_t queue_index)
{
	if (virtio_device_live(device)) device->ops->notify(device, queue_index);
}

static inline uint32_t virtio_device_interrupt_status(const virtio_device_t *device)
{
	return virtio_device_live(device) ? device->ops->interrupt_status(device) : 0U;
}

static inline void virtio_device_interrupt_ack(virtio_device_t *device, uint32_t status)
{
	if (virtio_device_live(device)) device->ops->interrupt_ack(device, status);
}

static inline bool virtio_device_irq_attach(virtio_device_t *device, irq_handler_t handler, void *context)
{
	return virtio_device_live(device) && device->ops->irq_attach(device, handler, context);
}

static inline uint8_t virtio_device_config_read8(const virtio_device_t *device, uint32_t offset)
{
	return virtio_device_live(device) ? device->ops->config_read8(device, offset) : 0U;
}

static inline uint32_t virtio_device_config_read32(const virtio_device_t *device, uint32_t offset)
{
	return virtio_device_live(device) ? device->ops->config_read32(device, offset) : 0U;
}

static inline uint64_t virtio_device_config_read64(const virtio_device_t *device, uint32_t offset)
{
	return virtio_device_live(device) ? device->ops->config_read64(device, offset) : 0ULL;
}

static inline void virtio_device_config_write8(virtio_device_t *device, uint32_t offset, uint8_t value)
{
	if (virtio_device_live(device)) device->ops->config_write8(device, offset, value);
}

#endif
