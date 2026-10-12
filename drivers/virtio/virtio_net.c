/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 *
 * VirtIO Ethernet: one RX/TX pair, software checksums, bounded DMA buffers.
 * Protocol processing belongs to the network worker, never the interrupt handler.
 */
#include <drivers/virtio/virtio_net.h>

#include <kern/console/console.h>
#include <kern/lock.h>
#include <kern/machine/barrier.h>
#include <kern/net/netif.h>
#include <vm/pmm.h>
#include <vm/vmm.h>
#include <string.h>

#define VIRTIO_NET_SLOTS 16U
#define VIRTIO_NET_QUEUE_SIZE (VIRTIO_NET_SLOTS * 2U)
#define VIRTIO_NET_FRAME_MAX 1514U
#define VIRTIO_NET_F_MAC (1ULL << 5)
#define VIRTIO_NET_F_STATUS (1ULL << 16)

typedef struct {
	uint64_t physical;
	uint8_t *bytes;
	uint16_t head;
	uint16_t tail;
	bool busy;
} virtio_net_buffer_t;

typedef struct {
	virtio_device_t transport;
	virtqueue_t receive;
	virtqueue_t transmit;
	virtio_net_buffer_t rx[VIRTIO_NET_SLOTS];
	virtio_net_buffer_t tx[VIRTIO_NET_SLOTS];
	netif_t *interface;
	nxu_spinlock_t lock;
	uint32_t header_length;
	bool attached;
	bool failed;
} virtio_net_device_t;

static virtio_net_device_t g_devices[NETIF_MAX - 1];
static uint32_t g_device_count;

static bool
virtio_net_allocate(virtio_net_buffer_t *buffer)
{
	if (!pmm_allocate_page(&buffer->physical)) {
		return false;
	}

	uint64_t address;

	if (!vmm_physical_to_higher_half(buffer->physical, &address)) {
		(void)pmm_free_page(buffer->physical);
		buffer->physical = 0;
		return false;
	}

	buffer->bytes = (void *)(uintptr_t)address;
	memset(buffer->bytes, 0, PMM_PAGE_SIZE);
	return true;
}

static bool
virtio_net_describe(virtio_net_device_t *device, virtqueue_t *queue, virtio_net_buffer_t *buffer, uint32_t length,
					bool receive)
{
	if (!virtqueue_alloc_descriptor(queue, &buffer->head)) {
		return false;
	}

	if (!virtqueue_alloc_descriptor(queue, &buffer->tail)) {
		(void)virtqueue_free_descriptor(queue, buffer->head);
		return false;
	}

	uint16_t flags = receive ? VIRTQ_DESC_F_WRITE : 0;
	queue->descriptors[buffer->head] = (virtq_desc_t){.address = buffer->physical,
													  .length = device->header_length,
													  .flags = flags | VIRTQ_DESC_F_NEXT,
													  .next = buffer->tail};
	queue->descriptors[buffer->tail] =
		(virtq_desc_t){.address = buffer->physical + device->header_length, .length = length, .flags = flags};

	buffer->busy = true;
	return virtqueue_submit(queue, buffer->head);
}

static void
virtio_net_fault(virtio_net_device_t *device)
{
	device->failed = true;
	device->interface->info.flags &= ~NXU_NET_IF_UP;
	virtio_device_fail(&device->transport);
	/* DMA storage remains pinned until the device can be reset. */
}

static void
virtio_net_complete_tx(virtio_net_device_t *device)
{
	for (uint32_t budget = 0; budget < VIRTIO_NET_SLOTS; budget++) {
		uint32_t id;
		uint32_t length;

		if (!virtqueue_pop_used(&device->transmit, &id, &length)) {
			return;
		}

		virtio_net_buffer_t *buffer = 0;

		for (uint32_t slot = 0; slot < VIRTIO_NET_SLOTS; slot++) {
			if (device->tx[slot].busy && device->tx[slot].head == id) {
				buffer = &device->tx[slot];
				break;
			}
		}

		if (buffer == 0) {
			virtio_net_fault(device);
			return;
		}

		(void)virtqueue_free_descriptor(&device->transmit, buffer->head);
		(void)virtqueue_free_descriptor(&device->transmit, buffer->tail);
		buffer->busy = false;
	}
}

static bool
virtio_net_transmit(void *context, const void *frame, uint32_t length)
{
	virtio_net_device_t *device = context;
	uint64_t irq = nxu_spin_lock_irqsave(&device->lock);
	bool sent = false;

	if (!device->attached || device->failed || length < 14 || length > VIRTIO_NET_FRAME_MAX) {
		goto done;
	}

	virtio_net_complete_tx(device);

	if (device->failed) {
		goto done;
	}

	for (uint32_t slot = 0; slot < VIRTIO_NET_SLOTS; slot++) {
		virtio_net_buffer_t *buffer = &device->tx[slot];

		if (!buffer->busy) {
			memset(buffer->bytes, 0, device->header_length);
			memcpy(buffer->bytes + device->header_length, frame, length);

			/* Ethernet frames require 60 bytes before the hardware FCS. */
			if (length < 60) {
				memset(buffer->bytes + device->header_length + length, 0, 60 - length);
				length = 60;
			}

			sent = virtio_net_describe(device, &device->transmit, buffer, length, false);
			if (sent) {
				virtio_device_notify(&device->transport, 1);
			}
			break;
		}
	}

done:
	nxu_spin_unlock_irqrestore(&device->lock, irq);
	return sent;
}

static void
virtio_net_poll(void *context)
{
	virtio_net_device_t *device = context;
	uint64_t irq = nxu_spin_lock_irqsave(&device->lock);
	bool recycled = false;

	if (!device->attached || device->failed) {
		goto done;
	}

	uint32_t status = virtio_device_interrupt_status(&device->transport);
	virtio_device_interrupt_ack(&device->transport, status);
	virtio_net_complete_tx(device);

	if (device->failed) {
		goto done;
	}

	for (uint32_t budget = 0; budget < VIRTIO_NET_SLOTS; budget++) {
		uint32_t id;
		uint32_t length;

		if (!virtqueue_pop_used(&device->receive, &id, &length)) {
			break;
		}

		virtio_net_buffer_t *buffer = 0;

		for (uint32_t slot = 0; slot < VIRTIO_NET_SLOTS; slot++) {
			if (device->rx[slot].head == id) {
				buffer = &device->rx[slot];
				break;
			}
		}

		if (buffer == 0) {
			virtio_net_fault(device);
			goto done;
		}

		ml_dma_rmb();
		bool valid = length >= device->header_length + 14 && length <= device->header_length + VIRTIO_NET_FRAME_MAX &&
					 buffer->bytes[0] == 0 && buffer->bytes[1] == 0;

		/* num_buffers is meaningful only with negotiated MRG_RXBUF. */

		netbuf_t *packet = valid ? netbuf_alloc() : 0;

		if (packet != 0) {
			void *bytes = netbuf_append(packet, length - device->header_length);
			memcpy(bytes, buffer->bytes + device->header_length, length - device->header_length);
			netif_input(device->interface, packet);
		} else {
			__atomic_fetch_add(&device->interface->info.dropped, 1, __ATOMIC_RELAXED);
		}

		memset(buffer->bytes, 0, device->header_length);
		if (!virtqueue_submit(&device->receive, buffer->head)) {
			virtio_net_fault(device);
			goto done;
		}

		recycled = true;
	}

	if (recycled) {
		virtio_device_notify(&device->transport, 0);
	}

done:
	nxu_spin_unlock_irqrestore(&device->lock, irq);
}

static void
virtio_net_irq(uint32_t intid, void *context)
{
	(void)intid;
	virtio_net_device_t *device = context;
	uint32_t status = virtio_device_interrupt_status(&device->transport);
	virtio_device_interrupt_ack(&device->transport, status);
}

bool
virtio_net_attach(const virtio_device_t *transport)
{
	if (g_device_count == NETIF_MAX - 1) {
		return false;
	}

	virtio_net_device_t *device = &g_devices[g_device_count];
	memset(device, 0, sizeof(*device));
	device->transport = *transport;

	if (!virtio_device_begin(&device->transport, VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS) ||
		!(device->transport.driver_features & VIRTIO_NET_F_MAC)) {
		goto fail;
	}

	device->header_length = (device->transport.driver_features & (1ULL << 32)) ? 12 : 10;

	if (!virtio_device_queue_init(&device->transport, 0, VIRTIO_NET_QUEUE_SIZE, &device->receive) ||
		!virtio_device_queue_init(&device->transport, 1, VIRTIO_NET_QUEUE_SIZE, &device->transmit)) {
		goto fail;
	}

	/* The worker polls every 2 ms, including on transports without usable IRQs. */
	*device->receive.available_flags = 1;
	*device->transmit.available_flags = 1;

	for (uint32_t slot = 0; slot < VIRTIO_NET_SLOTS; slot++) {
		if (!virtio_net_allocate(&device->rx[slot]) || !virtio_net_allocate(&device->tx[slot]) ||
			!virtio_net_describe(device, &device->receive, &device->rx[slot], VIRTIO_NET_FRAME_MAX, true)) {
			goto fail;
		}
	}

	if (!virtio_device_setup_queue(&device->transport, 0, &device->receive) ||
		!virtio_device_setup_queue(&device->transport, 1, &device->transmit) ||
		!virtio_device_finish(&device->transport)) {
		goto fail;
	}

	uint8_t mac[6];

	for (uint32_t byte = 0; byte < 6; byte++) {
		mac[byte] = virtio_device_config_read8(&device->transport, byte);
	}

	if ((mac[0] & 1) || memcmp(mac, (uint8_t[6]){0}, 6) == 0) {
		goto fail;
	}

	device->interface = netif_register(mac, device, virtio_net_transmit, virtio_net_poll);

	if (device->interface == 0) {
		goto fail;
	}

	device->attached = true;
	g_device_count++;
	(void)virtio_device_irq_attach(&device->transport, virtio_net_irq, device);
	virtio_device_notify(&device->transport, 0);
	kprintf("VirtIONetFamily: %s MAC %x:%x:%x:%x:%x:%x (%s)\n", device->interface->info.name, mac[0], mac[1], mac[2],
			mac[3], mac[4], mac[5], transport->ops->name);
	return true;

fail:
	virtio_device_fail(&device->transport);

	if (virtio_device_reset(&device->transport)) {
		(void)virtqueue_destroy(&device->receive);
		(void)virtqueue_destroy(&device->transmit);

		for (uint32_t slot = 0; slot < VIRTIO_NET_SLOTS; slot++) {
			if (device->rx[slot].physical != 0) {
				(void)pmm_free_page(device->rx[slot].physical);
			}
			if (device->tx[slot].physical != 0) {
				(void)pmm_free_page(device->tx[slot].physical);
			}
		}
	} else {
		/* Reserve this slot rather than overwrite memory still owned by a device. */
		g_device_count++;
	}

	return false;
}
