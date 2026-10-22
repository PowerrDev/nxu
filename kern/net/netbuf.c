/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/net/netbuf.c
 *
 * Implements packet buffer allocation and manipulation.
 */
#include <kern/net/netbuf.h>

#include <kern/lock.h>

static netbuf_t g_packets[NETBUF_COUNT];
static nxu_spinlock_t g_pool_lock;

netbuf_t *
netbuf_alloc(void)
{
	uint64_t irq = nxu_spin_lock_irqsave(&g_pool_lock);

	for (uint32_t index = 0; index < NETBUF_COUNT; index++) {
		netbuf_t *packet = &g_packets[index];

		if (!packet->allocated) {
			packet->allocated = true;
			packet->next = 0;
			packet->interface = 0;
			packet->offset = NETBUF_HEADROOM;
			packet->length = 0;
			nxu_spin_unlock_irqrestore(&g_pool_lock, irq);
			return packet;
		}

	}

	nxu_spin_unlock_irqrestore(&g_pool_lock, irq);
	return 0;
}

void
netbuf_free(netbuf_t *packet)
{
	if (packet == 0) return;

	uint64_t irq = nxu_spin_lock_irqsave(&g_pool_lock);
	packet->allocated = false;
	nxu_spin_unlock_irqrestore(&g_pool_lock, irq);
}

uint8_t *
netbuf_data(netbuf_t *packet)
{
	return packet->storage + packet->offset;
}

void *
netbuf_append(netbuf_t *packet, uint32_t length)
{
	if (length > NETBUF_CAPACITY - packet->offset - packet->length) return 0;

	void *tail = netbuf_data(packet) + packet->length;
	packet->length += length;
	return tail;
}

void *
netbuf_prepend(netbuf_t *packet, uint32_t length)
{
	if (length > packet->offset) return 0;

	packet->offset -= length;
	packet->length += length;
	return netbuf_data(packet);
}

bool
netbuf_pull(netbuf_t *packet, uint32_t length)
{
	if (length > packet->length) return false;

	packet->offset += length;
	packet->length -= length;
	return true;
}
