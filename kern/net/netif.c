/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/net/netif.c
 *
 * Registers network interfaces and manages their state.
 */
#include <kern/net/netif.h>

#include <kern/lock.h>
#include <string.h>

static netif_t g_interfaces[NETIF_MAX];
static uint32_t g_interface_count;
static netbuf_t *g_receive_head;
static netbuf_t *g_receive_tail;
static nxu_spinlock_t g_receive_lock;

void
netif_initialize(void)
{
	if (g_interface_count != 0) return;

	netif_t *loopback = &g_interfaces[g_interface_count++];
    
	memcpy(loopback->info.name, "lo0", 4);
	
    loopback->info.index = 1;
	loopback->info.flags = NXU_NET_IF_UP | NXU_NET_IF_LOOPBACK;
	loopback->info.mtu = 1500;
	loopback->info.prefix4 = 8;
	loopback->info.address4[0] = 127;
	loopback->info.address4[3] = 1;
	loopback->info.address6[15] = 1;
	loopback->info.link_local6[15] = 1;
}

netif_t *
netif_register(const uint8_t mac[6], void *context, bool (*transmit)(void *, const void *, uint32_t),
			   void (*poll)(void *))
{
	netif_initialize();

	if (g_interface_count == NETIF_MAX || transmit == 0 || poll == 0) return 0;

	netif_t *interface = &g_interfaces[g_interface_count++];
	interface->context = context;
	interface->transmit = transmit;
	interface->poll = poll;
	interface->info.index = g_interface_count;
	interface->info.flags = NXU_NET_IF_UP | NXU_NET_IF_TENTATIVE;
	interface->info.mtu = 1500;
    
	memcpy(interface->info.name, "en0", 4);
	interface->info.name[2] += g_interface_count - 2;
	memcpy(interface->info.mac, mac, 6);

	/* QEMU user-network defaults; secondary interfaces have no default route. */
	if (g_interface_count == 2) {
		const uint8_t address[4] = {10, 0, 2, 15};
		const uint8_t gateway[4] = {10, 0, 2, 2};
	
        memcpy(interface->info.address4, address, 4);
		memcpy(interface->info.gateway4, gateway, 4);
		
        interface->info.prefix4 = 24;
		interface->info.address6[0] = 0xfe;
		interface->info.address6[1] = 0xc0;
		interface->info.address6[15] = 0x15;
	}

	uint8_t *link = interface->info.link_local6;
    
	link[0] = 0xfe;
	link[1] = 0x80;
	link[8] = mac[0] ^ 2;
	link[9] = mac[1];
	link[10] = mac[2];
	link[11] = 0xff;
	link[12] = 0xfe;
	link[13] = mac[3];
	link[14] = mac[4];
	link[15] = mac[5];

	return interface;
}

netif_t *
netif_at(uint32_t index)
{
	return index < g_interface_count ? &g_interfaces[index] : 0;
}

uint32_t
netif_count(void)
{
	return g_interface_count;
}

void
netif_input(netif_t *interface, netbuf_t *packet)
{
	packet->interface = interface;
	packet->next = 0;

	uint64_t irq = nxu_spin_lock_irqsave(&g_receive_lock);

	if (g_receive_tail != 0) {
		g_receive_tail->next = packet;
	} else {
		g_receive_head = packet;
	}

	g_receive_tail = packet;
	__atomic_fetch_add(&interface->info.received, 1, __ATOMIC_RELAXED);
    
	nxu_spin_unlock_irqrestore(&g_receive_lock, irq);
}

netbuf_t *
netif_dequeue(void)
{
	uint64_t irq = nxu_spin_lock_irqsave(&g_receive_lock);
	netbuf_t *packet = g_receive_head;

	if (packet != 0) {
		g_receive_head = packet->next;
		if (g_receive_head == 0) {
			g_receive_tail = 0;
		}
	}

	nxu_spin_unlock_irqrestore(&g_receive_lock, irq);
    
	return packet;
}

bool
netif_output(netif_t *interface, netbuf_t *packet)
{
	if (interface->info.flags & NXU_NET_IF_LOOPBACK) {
		__atomic_fetch_add(&interface->info.transmitted, 1, __ATOMIC_RELAXED);
		netif_input(interface, packet);
		return true;
	}

	if (packet->length >= 14 && memcmp(netbuf_data(packet), interface->info.mac, 6) == 0) {
		__atomic_fetch_add(&interface->info.transmitted, 1, __ATOMIC_RELAXED);
		netif_input(interface, packet);
        
		return true;
	}

	bool sent = interface->transmit(interface->context, netbuf_data(packet), packet->length);
	
    if (sent) {
		__atomic_fetch_add(&interface->info.transmitted, 1, __ATOMIC_RELAXED);
	} else {
		__atomic_fetch_add(&interface->info.dropped, 1, __ATOMIC_RELAXED);
	}
	
    netbuf_free(packet);
    
	return sent;
}
