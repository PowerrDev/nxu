/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/virtio/virtio_pci.c
 *
 * VirtIO over PCI: the legacy (I/O-port) and modern (capability) transports
 * behind the transport interface of virtio_transport.h, plus the bus scanner
 * that turns enumerated PCI functions into virtio_device_t objects for
 * virtio_init().
 *
 * Legacy layout (virtio 0.9.5, I/O BAR0):
 *
 *   0x00 device features   0x08 queue PFN     0x12 device status
 *   0x04 driver features   0x0C queue size    0x13 ISR (read clears)
 *   0x0E queue select      0x10 queue notify  0x14 device configuration
 *
 * There is no FEATURES_OK step and only 32 feature bits; the used ring sits on
 * the next 4096-byte boundary after the available ring; the device fixes the
 * queue size. Modern devices publish the same things through vendor
 * capabilities (common, notify, ISR, device) that point into memory BARs.
 */

#include <drivers/virtio/virtio_pci.h>

#include <kern/console/console.h>
#include <kern/irq/irq.h>
#include <kern/machine/barrier.h>
#include <platform/i386/portio.h>

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_LEGACY_HOST_FEATURES 0x00U
#define VIRTIO_LEGACY_GUEST_FEATURES 0x04U
#define VIRTIO_LEGACY_QUEUE_PFN 0x08U
#define VIRTIO_LEGACY_QUEUE_NUM 0x0CU
#define VIRTIO_LEGACY_QUEUE_SEL 0x0EU
#define VIRTIO_LEGACY_QUEUE_NOTIFY 0x10U
#define VIRTIO_LEGACY_STATUS 0x12U
#define VIRTIO_LEGACY_ISR 0x13U
#define VIRTIO_LEGACY_CONFIG 0x14U

#define VIRTIO_LEGACY_PAGE_SHIFT 12U

#define VIRTIO_PCI_CAP_COMMON 1U
#define VIRTIO_PCI_CAP_NOTIFY 2U
#define VIRTIO_PCI_CAP_ISR 3U
#define VIRTIO_PCI_CAP_DEVICE 4U

#define VIRTIO_COMMON_DEVICE_FEATURE_SELECT 0x00U
#define VIRTIO_COMMON_DEVICE_FEATURE 0x04U
#define VIRTIO_COMMON_DRIVER_FEATURE_SELECT 0x08U
#define VIRTIO_COMMON_DRIVER_FEATURE 0x0CU
#define VIRTIO_COMMON_STATUS 0x14U
#define VIRTIO_COMMON_CONFIG_GENERATION 0x15U
#define VIRTIO_COMMON_QUEUE_SELECT 0x16U
#define VIRTIO_COMMON_QUEUE_SIZE 0x18U
#define VIRTIO_COMMON_QUEUE_ENABLE 0x1CU
#define VIRTIO_COMMON_QUEUE_NOTIFY_OFF 0x1EU
#define VIRTIO_COMMON_QUEUE_DESC 0x20U
#define VIRTIO_COMMON_QUEUE_DRIVER 0x28U
#define VIRTIO_COMMON_QUEUE_DEVICE 0x30U
#define VIRTIO_COMMON_LENGTH 0x38U

#define VIRTIO_PCI_RESET_SPINS 1000000U

static bool g_virtio_pci_irq_mode;
static uint32_t g_virtio_pci_irq_bound;

void virtio_pci_set_irq_mode(bool enabled)
{
	g_virtio_pci_irq_mode = enabled;
}

bool virtio_pci_irq_mode(void)
{
	return g_virtio_pci_irq_mode;
}

uint32_t virtio_pci_irq_bound_count(void)
{
	return g_virtio_pci_irq_bound;
}

/*
 * PIC hooks from the interrupts area (kern/i386/pic.h), weak so this file
 * links before that area is merged. In that case irq_register() refuses too,
 * so the drivers poll.
 */
extern void pic_unmask(uint32_t irq) __attribute__((weak));
extern bool pic_set_level_triggered(uint32_t irq, bool level) __attribute__((weak));

/*
 * virtio_pci_irq_attach:
 *
 * Shared by both PCI transports. Polling is the default and needs nothing
 * here. With interrupt delivery enabled the handler is chained onto the PIC
 * line the firmware wrote into the PCI interrupt-line register (INTx lines
 * are shared, so several devices may register on one line and each handler
 * checks its own ISR byte), the line is switched to level triggering in the
 * ELCR as PCI requires, and then unmasked. A line the PIC cannot serve or a
 * refused registration is not fatal because the drivers can always poll.
 */
static bool virtio_pci_irq_attach(virtio_device_t *device, irq_handler_t handler, void *context)
{
	if (!g_virtio_pci_irq_mode) return true;

	uint8_t line = device->pci.interrupt_line;

	if (line >= 16U) {
		kprintf("virtio_pci_irq_attach: no usable interrupt line (%u), polling\n", (unsigned int)line);
		return true;
	}

	if (!irq_register(line, handler, context)) {
		kprintf("virtio_pci_irq_attach: irq_register(%u) refused, polling\n", (unsigned int)line);
		return true;
	}

	if (pic_set_level_triggered != 0 && !pic_set_level_triggered(line, true)) {
		kprintf("virtio_pci_irq_attach: line %u stays edge triggered\n", (unsigned int)line);
	}

	if (pic_unmask != 0) pic_unmask(line);

	g_virtio_pci_irq_bound++;
	kprintf("virtio_pci_irq_attach: handler chained on IRQ %u\n", (unsigned int)line);
	return true;
}

/* ---- legacy transport ------------------------------------------------- */

static uint16_t virtio_legacy_port(const virtio_device_t *device, uint16_t offset)
{
	return (uint16_t)(device->pci.io_base + offset);
}

static bool virtio_legacy_reset(virtio_device_t *device)
{
	outb(virtio_legacy_port(device, VIRTIO_LEGACY_STATUS), 0U);
	ml_dma_mb();

	for (uint32_t spin = 0U; inb(virtio_legacy_port(device, VIRTIO_LEGACY_STATUS)) != 0U; spin++) {
		if (spin >= VIRTIO_PCI_RESET_SPINS) return false;
		ml_cpu_relax();
	}

	device->device_features = 0ULL;
	device->driver_features = 0ULL;
	device->initialized = false;
	device->driver_ok = false;
	return true;
}

static void virtio_legacy_fail(virtio_device_t *device)
{
	uint16_t port = virtio_legacy_port(device, VIRTIO_LEGACY_STATUS);

	outb(port, (uint8_t)(inb(port) | VIRTIO_STATUS_FAILED));
	ml_dma_mb();
}

/*
 * Legacy negotiation: ACKNOWLEDGE, DRIVER, read the 32 device feature bits,
 * accept the class driver's subset. The device has no FEATURES_OK handshake,
 * so the negotiation is complete once the driver bits are written.
 */
static bool virtio_legacy_begin(virtio_device_t *device, uint64_t accepted_device_features)
{
	if (device->device_id == VIRTIO_DEVICE_ID_NONE || device->initialized) return false;
	if (!virtio_legacy_reset(device)) return false;

	uint16_t status_port = virtio_legacy_port(device, VIRTIO_LEGACY_STATUS);

	outb(status_port, VIRTIO_STATUS_ACKNOWLEDGE);
	outb(status_port, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

	uint32_t offered = inl(virtio_legacy_port(device, VIRTIO_LEGACY_HOST_FEATURES));
	uint32_t accepted = offered & (uint32_t)accepted_device_features;

	device->device_features = offered;
	device->driver_features = accepted;

	outl(virtio_legacy_port(device, VIRTIO_LEGACY_GUEST_FEATURES), accepted);
	ml_dma_mb();

	device->initialized = true;
	return true;
}

/*
 * The legacy queue size is read-only: the device says how many descriptors the
 * ring has and the driver lays the ring out for exactly that many.
 */
static bool virtio_legacy_queue_init(virtio_device_t *device, uint16_t queue_index, uint16_t size, virtqueue_t *queue)
{
	(void)size;

	if (!device->initialized) return false;

	outw(virtio_legacy_port(device, VIRTIO_LEGACY_QUEUE_SEL), queue_index);

	uint16_t device_size = inw(virtio_legacy_port(device, VIRTIO_LEGACY_QUEUE_NUM));

	if (device_size == 0U) return false;
	if (inl(virtio_legacy_port(device, VIRTIO_LEGACY_QUEUE_PFN)) != 0U) return false;

	return virtqueue_init_legacy(queue, device_size);
}

static bool virtio_legacy_setup_queue(virtio_device_t *device, uint16_t queue_index, virtqueue_t *queue)
{
	if (!device->initialized || !queue->initialized) return false;

	outw(virtio_legacy_port(device, VIRTIO_LEGACY_QUEUE_SEL), queue_index);

	if (inw(virtio_legacy_port(device, VIRTIO_LEGACY_QUEUE_NUM)) != queue->size) return false;

	uint64_t frame = virtqueue_desc_physical(queue) >> VIRTIO_LEGACY_PAGE_SHIFT;

	if (frame == 0ULL || frame > 0xFFFFFFFFULL) return false;

	ml_dma_mb();
	outl(virtio_legacy_port(device, VIRTIO_LEGACY_QUEUE_PFN), (uint32_t)frame);
	ml_dma_mb();

	return inl(virtio_legacy_port(device, VIRTIO_LEGACY_QUEUE_PFN)) == (uint32_t)frame;
}

static bool virtio_legacy_finish(virtio_device_t *device)
{
	if (!device->initialized || device->driver_ok) return false;

	uint16_t port = virtio_legacy_port(device, VIRTIO_LEGACY_STATUS);

	outb(port, (uint8_t)(inb(port) | VIRTIO_STATUS_DRIVER_OK));
	ml_dma_mb();

	if ((inb(port) & VIRTIO_STATUS_DRIVER_OK) == 0U) {
		virtio_legacy_fail(device);
		return false;
	}

	device->driver_ok = true;
	return true;
}

static void virtio_legacy_notify(virtio_device_t *device, uint16_t queue_index)
{
	if (!device->driver_ok) return;

	ml_dma_mb();
	outw(virtio_legacy_port(device, VIRTIO_LEGACY_QUEUE_NOTIFY), queue_index);
}

/* Reading the ISR byte acknowledges it, so it is read exactly once per interrupt. */
static uint32_t virtio_legacy_interrupt_status(const virtio_device_t *device)
{
	if (!device->initialized) return 0U;

	return inb(virtio_legacy_port(device, VIRTIO_LEGACY_ISR));
}

static void virtio_legacy_interrupt_ack(virtio_device_t *device, uint32_t status)
{
	(void)device;
	(void)status;
}

static uint8_t virtio_legacy_config_read8(const virtio_device_t *device, uint32_t offset)
{
	if (!device->initialized) return 0U;

	return inb(virtio_legacy_port(device, (uint16_t)(VIRTIO_LEGACY_CONFIG + offset)));
}

static uint32_t virtio_legacy_config_read32(const virtio_device_t *device, uint32_t offset)
{
	uint32_t value = 0U;

	for (uint32_t byte = 0U; byte < 4U; byte++) {
		value |= (uint32_t)virtio_legacy_config_read8(device, offset + byte) << (byte * 8U);
	}

	return value;
}

static uint64_t virtio_legacy_config_read64(const virtio_device_t *device, uint32_t offset)
{
	uint64_t value = 0ULL;

	for (uint32_t byte = 0U; byte < 8U; byte++) {
		value |= (uint64_t)virtio_legacy_config_read8(device, offset + byte) << (byte * 8U);
	}

	return value;
}

static void virtio_legacy_config_write8(virtio_device_t *device, uint32_t offset, uint8_t value)
{
	if (!device->initialized) return;

	outb(virtio_legacy_port(device, (uint16_t)(VIRTIO_LEGACY_CONFIG + offset)), value);
	ml_dma_mb();
}

const virtio_transport_ops_t virtio_pci_legacy_ops = {
	.name = "virtio-pci-legacy",
	.begin = virtio_legacy_begin,
	.queue_init = virtio_legacy_queue_init,
	.setup_queue = virtio_legacy_setup_queue,
	.finish = virtio_legacy_finish,
	.reset = virtio_legacy_reset,
	.fail = virtio_legacy_fail,
	.notify = virtio_legacy_notify,
	.interrupt_status = virtio_legacy_interrupt_status,
	.interrupt_ack = virtio_legacy_interrupt_ack,
	.irq_attach = virtio_pci_irq_attach,
	.config_read8 = virtio_legacy_config_read8,
	.config_read32 = virtio_legacy_config_read32,
	.config_read64 = virtio_legacy_config_read64,
	.config_write8 = virtio_legacy_config_write8
};

/* ---- modern transport ------------------------------------------------- */

static volatile uint8_t *virtio_common(const virtio_device_t *device, uint32_t offset)
{
	return device->pci.common + offset;
}

static uint32_t virtio_modern_read32(const virtio_device_t *device, uint32_t offset)
{
	return *(volatile uint32_t *)virtio_common(device, offset);
}

static void virtio_modern_write32(virtio_device_t *device, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)virtio_common(device, offset) = value;
}

static uint16_t virtio_modern_read16(const virtio_device_t *device, uint32_t offset)
{
	return *(volatile uint16_t *)virtio_common(device, offset);
}

static void virtio_modern_write16(virtio_device_t *device, uint32_t offset, uint16_t value)
{
	*(volatile uint16_t *)virtio_common(device, offset) = value;
}

static uint8_t virtio_modern_status(const virtio_device_t *device)
{
	return *virtio_common(device, VIRTIO_COMMON_STATUS);
}

static void virtio_modern_set_status(virtio_device_t *device, uint8_t status)
{
	*virtio_common(device, VIRTIO_COMMON_STATUS) = status;
	ml_dma_mb();
}

static bool virtio_modern_reset(virtio_device_t *device)
{
	virtio_modern_set_status(device, 0U);

	for (uint32_t spin = 0U; virtio_modern_status(device) != 0U; spin++) {
		if (spin >= VIRTIO_PCI_RESET_SPINS) return false;
		ml_cpu_relax();
	}

	device->device_features = 0ULL;
	device->driver_features = 0ULL;
	device->initialized = false;
	device->driver_ok = false;
	return true;
}

static void virtio_modern_fail(virtio_device_t *device)
{
	virtio_modern_set_status(device, (uint8_t)(virtio_modern_status(device) | VIRTIO_STATUS_FAILED));
}

static uint64_t virtio_modern_read_features(virtio_device_t *device)
{
	virtio_modern_write32(device, VIRTIO_COMMON_DEVICE_FEATURE_SELECT, 0U);
	ml_dma_mb();

	uint64_t low = virtio_modern_read32(device, VIRTIO_COMMON_DEVICE_FEATURE);

	virtio_modern_write32(device, VIRTIO_COMMON_DEVICE_FEATURE_SELECT, 1U);
	ml_dma_mb();

	uint64_t high = virtio_modern_read32(device, VIRTIO_COMMON_DEVICE_FEATURE);

	return low | (high << 32U);
}

static bool virtio_modern_begin(virtio_device_t *device, uint64_t accepted_device_features)
{
	if (device->device_id == VIRTIO_DEVICE_ID_NONE || device->initialized) return false;
	if (!virtio_modern_reset(device)) return false;

	uint8_t status = VIRTIO_STATUS_ACKNOWLEDGE;

	virtio_modern_set_status(device, status);
	status |= VIRTIO_STATUS_DRIVER;
	virtio_modern_set_status(device, status);

	device->device_features = virtio_modern_read_features(device);

	uint64_t version_feature = 1ULL << VIRTIO_F_VERSION_1;

	if ((device->device_features & version_feature) == 0ULL) {
		virtio_modern_fail(device);
		return false;
	}

	device->driver_features = (device->device_features & (accepted_device_features | version_feature)) | version_feature;

	virtio_modern_write32(device, VIRTIO_COMMON_DRIVER_FEATURE_SELECT, 0U);
	virtio_modern_write32(device, VIRTIO_COMMON_DRIVER_FEATURE, (uint32_t)device->driver_features);
	virtio_modern_write32(device, VIRTIO_COMMON_DRIVER_FEATURE_SELECT, 1U);
	virtio_modern_write32(device, VIRTIO_COMMON_DRIVER_FEATURE, (uint32_t)(device->driver_features >> 32U));

	status |= VIRTIO_STATUS_FEATURES_OK;
	virtio_modern_set_status(device, status);

	if ((virtio_modern_status(device) & VIRTIO_STATUS_FEATURES_OK) == 0U) {
		virtio_modern_fail(device);
		return false;
	}

	device->initialized = true;
	return true;
}

/* Modern devices let the driver pick any power of two up to the device maximum. */
static bool virtio_modern_queue_init(virtio_device_t *device, uint16_t queue_index, uint16_t size, virtqueue_t *queue)
{
	if (!device->initialized) return false;

	virtio_modern_write16(device, VIRTIO_COMMON_QUEUE_SELECT, queue_index);
	ml_dma_mb();

	uint16_t maximum = virtio_modern_read16(device, VIRTIO_COMMON_QUEUE_SIZE);

	if (maximum == 0U || size > maximum) return false;

	return virtqueue_init(queue, size);
}

static void virtio_modern_write64(virtio_device_t *device, uint32_t offset, uint64_t value)
{
	virtio_modern_write32(device, offset, (uint32_t)value);
	virtio_modern_write32(device, offset + 4U, (uint32_t)(value >> 32U));
}

static bool virtio_modern_setup_queue(virtio_device_t *device, uint16_t queue_index, virtqueue_t *queue)
{
	if (!device->initialized || !queue->initialized || queue_index >= VIRTIO_PCI_MAX_QUEUES) return false;

	virtio_modern_write16(device, VIRTIO_COMMON_QUEUE_SELECT, queue_index);
	ml_dma_mb();

	uint16_t maximum = virtio_modern_read16(device, VIRTIO_COMMON_QUEUE_SIZE);

	if (maximum == 0U || queue->size > maximum) return false;
	if (virtio_modern_read16(device, VIRTIO_COMMON_QUEUE_ENABLE) != 0U) return false;

	virtio_modern_write16(device, VIRTIO_COMMON_QUEUE_SIZE, queue->size);
	virtio_modern_write64(device, VIRTIO_COMMON_QUEUE_DESC, virtqueue_desc_physical(queue));
	virtio_modern_write64(device, VIRTIO_COMMON_QUEUE_DRIVER, virtqueue_available_physical(queue));
	virtio_modern_write64(device, VIRTIO_COMMON_QUEUE_DEVICE, virtqueue_used_physical(queue));

	device->pci.queue_notify_offset[queue_index] = virtio_modern_read16(device, VIRTIO_COMMON_QUEUE_NOTIFY_OFF);

	ml_dma_mb();
	virtio_modern_write16(device, VIRTIO_COMMON_QUEUE_ENABLE, 1U);
	ml_dma_mb();

	return virtio_modern_read16(device, VIRTIO_COMMON_QUEUE_ENABLE) == 1U;
}

static bool virtio_modern_finish(virtio_device_t *device)
{
	if (!device->initialized || device->driver_ok) return false;

	virtio_modern_set_status(device, (uint8_t)(virtio_modern_status(device) | VIRTIO_STATUS_DRIVER_OK));

	if ((virtio_modern_status(device) & VIRTIO_STATUS_DRIVER_OK) == 0U) {
		virtio_modern_fail(device);
		return false;
	}

	device->driver_ok = true;
	return true;
}

static void virtio_modern_notify(virtio_device_t *device, uint16_t queue_index)
{
	if (!device->driver_ok || queue_index >= VIRTIO_PCI_MAX_QUEUES) return;

	uint32_t offset = (uint32_t)device->pci.queue_notify_offset[queue_index] * device->pci.notify_multiplier;

	ml_dma_mb();
	*(volatile uint16_t *)(device->pci.notify + offset) = queue_index;
}

static uint32_t virtio_modern_interrupt_status(const virtio_device_t *device)
{
	if (!device->initialized) return 0U;

	return *device->pci.isr;
}

static void virtio_modern_interrupt_ack(virtio_device_t *device, uint32_t status)
{
	(void)device;
	(void)status;
}

static uint8_t virtio_modern_config_read8(const virtio_device_t *device, uint32_t offset)
{
	if (!device->initialized || device->pci.device_config == 0) return 0U;

	return device->pci.device_config[offset];
}

static uint32_t virtio_modern_config_read32(const virtio_device_t *device, uint32_t offset)
{
	if (!device->initialized) return 0U;

	for (;;) {
		uint8_t before = *virtio_common(device, VIRTIO_COMMON_CONFIG_GENERATION);
		uint32_t value = 0U;

		for (uint32_t byte = 0U; byte < 4U; byte++) {
			value |= (uint32_t)virtio_modern_config_read8(device, offset + byte) << (byte * 8U);
		}

		ml_dma_rmb();

		if (before == *virtio_common(device, VIRTIO_COMMON_CONFIG_GENERATION)) return value;
	}
}

static uint64_t virtio_modern_config_read64(const virtio_device_t *device, uint32_t offset)
{
	if (!device->initialized) return 0ULL;

	for (;;) {
		uint8_t before = *virtio_common(device, VIRTIO_COMMON_CONFIG_GENERATION);
		uint64_t value = 0ULL;

		for (uint32_t byte = 0U; byte < 8U; byte++) {
			value |= (uint64_t)virtio_modern_config_read8(device, offset + byte) << (byte * 8U);
		}

		ml_dma_rmb();

		if (before == *virtio_common(device, VIRTIO_COMMON_CONFIG_GENERATION)) return value;
	}
}

static void virtio_modern_config_write8(virtio_device_t *device, uint32_t offset, uint8_t value)
{
	if (!device->initialized || device->pci.device_config == 0) return;

	device->pci.device_config[offset] = value;
	ml_dma_mb();
}

const virtio_transport_ops_t virtio_pci_modern_ops = {
	.name = "virtio-pci-modern",
	.begin = virtio_modern_begin,
	.queue_init = virtio_modern_queue_init,
	.setup_queue = virtio_modern_setup_queue,
	.finish = virtio_modern_finish,
	.reset = virtio_modern_reset,
	.fail = virtio_modern_fail,
	.notify = virtio_modern_notify,
	.interrupt_status = virtio_modern_interrupt_status,
	.interrupt_ack = virtio_modern_interrupt_ack,
	.irq_attach = virtio_pci_irq_attach,
	.config_read8 = virtio_modern_config_read8,
	.config_read32 = virtio_modern_config_read32,
	.config_read64 = virtio_modern_config_read64,
	.config_write8 = virtio_modern_config_write8
};

/* ---- probing ---------------------------------------------------------- */

typedef struct {
	bool present;
	uint8_t bar;
	uint32_t offset;
	uint32_t length;
} virtio_pci_window_t;

/*
 * virtio_modern_map:
 *
 * Find the VirtIO vendor capabilities, map the windows they describe and fill
 * in the modern-transport state. False when a required capability is missing
 * or its BAR cannot be mapped (see pci_map_bar).
 */
static bool virtio_modern_map(const pci_device_t *pci, virtio_device_t *device)
{
	virtio_pci_window_t common = { 0 };
	virtio_pci_window_t notify = { 0 };
	virtio_pci_window_t isr = { 0 };
	virtio_pci_window_t config = { 0 };
	uint32_t multiplier = 0U;

	uint8_t id;
	uint8_t offset = 0U;

	/* The bound stops a corrupt, circular capability list from spinning forever. */
	for (uint32_t walked = 0U; walked < 48U && pci_capability_next(pci, offset, &id, &offset); walked++) {
		if (id != PCI_CAPABILITY_VENDOR) continue;

		uint8_t type = pci_config_read8(pci->bus, pci->slot, pci->function, (uint8_t)(offset + 3U));
		virtio_pci_window_t window = {
			.present = true,
			.bar = pci_config_read8(pci->bus, pci->slot, pci->function, (uint8_t)(offset + 4U)),
			.offset = pci_config_read32(pci->bus, pci->slot, pci->function, (uint8_t)(offset + 8U)),
			.length = pci_config_read32(pci->bus, pci->slot, pci->function, (uint8_t)(offset + 12U))
		};

		if (type == VIRTIO_PCI_CAP_COMMON && !common.present) common = window;
		else if (type == VIRTIO_PCI_CAP_NOTIFY && !notify.present) {
			notify = window;
			multiplier = pci_config_read32(pci->bus, pci->slot, pci->function, (uint8_t)(offset + 16U));
		} else if (type == VIRTIO_PCI_CAP_ISR && !isr.present) isr = window;
		else if (type == VIRTIO_PCI_CAP_DEVICE && !config.present) config = window;
	}

	if (!common.present || !notify.present || !isr.present || common.length < VIRTIO_COMMON_LENGTH) return false;

	device->pci.common = pci_map_bar(pci, common.bar, common.offset, common.length);
	device->pci.notify = pci_map_bar(pci, notify.bar, notify.offset, notify.length);
	device->pci.isr = pci_map_bar(pci, isr.bar, isr.offset, isr.length);
	device->pci.notify_multiplier = multiplier;

	if (config.present) device->pci.device_config = pci_map_bar(pci, config.bar, config.offset, config.length);

	if (device->pci.common == 0 || device->pci.notify == 0 || device->pci.isr == 0) return false;

	device->region.base = pci->bars[common.bar].base + common.offset;
	device->region.size = common.length;
	return true;
}

bool virtio_pci_probe(const pci_device_t *pci, virtio_device_t *device)
{
	if (pci == 0 || device == 0 || pci->vendor_id != VIRTIO_PCI_VENDOR_ID) return false;

	uint32_t device_id = pci->device_id;
	uint32_t type;
	bool transitional = false;

	if (device_id >= VIRTIO_PCI_DEVICE_ID_LEGACY_FIRST && device_id <= VIRTIO_PCI_DEVICE_ID_LEGACY_LAST) {
		type = pci->subsystem_id;
		transitional = true;
	} else if (device_id >= VIRTIO_PCI_DEVICE_ID_MODERN_FIRST && device_id <= VIRTIO_PCI_DEVICE_ID_MODERN_LAST) {
		type = device_id - VIRTIO_PCI_DEVICE_ID_MODERN_FIRST;
	} else {
		return false;
	}

	virtio_device_t probe = { 0 };

	probe.device_id = type;
	probe.vendor_id = pci->vendor_id;
	probe.intid = pci->interrupt_line;
	probe.irq_flags = 0U;
	probe.pci.bus = pci->bus;
	probe.pci.slot = pci->slot;
	probe.pci.function = pci->function;
	probe.pci.interrupt_line = pci->interrupt_line;

	if (transitional) {
		const pci_bar_t *bar = &pci->bars[0];

		if (!bar->valid || !bar->io || bar->base == 0ULL || bar->base > 0xFFFFULL) {
			kprintf("virtio_pci_probe: legacy device %x:%x has no I/O BAR0\n", (unsigned int)pci->vendor_id, (unsigned int)pci->device_id);
			return false;
		}

		probe.ops = &virtio_pci_legacy_ops;
		probe.pci.io_base = (uint16_t)bar->base;
		probe.region.base = bar->base;
		probe.region.size = bar->size;
		pci_command_update(pci, PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER, true);
	} else {
		probe.ops = &virtio_pci_modern_ops;
		pci_command_update(pci, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER, true);

		if (!virtio_modern_map(pci, &probe)) {
			kputs("virtio_pci_probe: modern device at ");
			pci_print_location(pci);
			kputln(" needs its memory BARs mapped (no MMIO mapping available)");
			return false;
		}
	}

	pci_command_update(pci, PCI_COMMAND_INTX_DISABLE, false);
	*device = probe;
	return true;
}

bool virtio_pci_scan(const platform_t *platform, const virtio_probe_policy_t *policy)
{
	(void)platform;

	for (uint32_t index = 0U; index < pci_device_count(); index++) {
		const pci_device_t *pci = pci_device_at(index);
		virtio_device_t device;

		if (!virtio_pci_probe(pci, &device)) continue;

		kputs("virtio_pci_scan: ");
		pci_print_location(pci);
		kprintf(
			" device ID %u via %s, irq line %u\n",
			device.device_id,
			device.ops->name,
			(unsigned int)pci->interrupt_line
		);

		if (!virtio_bind_device(&device, policy)) return false;
	}

	return true;
}

bool virtio_pci_register(void)
{
	return virtio_bus_register(virtio_pci_scan);
}
