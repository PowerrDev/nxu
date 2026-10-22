/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/net/arp.c
 *
 * Implements ARP neighbor discovery and address caching.
 */
#include <kern/net/protocol.h>

#define ARP_CACHE_SIZE 16U
#define ARP_LIFETIME_US 60000000ULL
#define ARP_RETRY_US 250000ULL

typedef struct {
	netif_t *interface;
	uint8_t address[4];
	uint8_t mac[6];
	uint64_t updated;
	uint64_t requested;
	bool valid;
} arp_entry_t;

/* Serialized by the protocol lock. No packet is retained while resolving a neighbor. */
static arp_entry_t g_neighbors[ARP_CACHE_SIZE];
static uint32_t g_replace;

static arp_entry_t *
arp_entry(netif_t *interface, const uint8_t address[4])
{
	for (uint32_t index = 0; index < ARP_CACHE_SIZE; index++) {
		arp_entry_t *entry = &g_neighbors[index];

		if (entry->interface == interface && memcmp(entry->address, address, 4) == 0) return entry;
	}

	arp_entry_t *entry = &g_neighbors[g_replace++ % ARP_CACHE_SIZE];
	memset(entry, 0, sizeof(*entry));
	entry->interface = interface;
	memcpy(entry->address, address, 4);
	return entry;
}

static bool
arp_send(netif_t *interface, uint16_t operation, const uint8_t address[4], const uint8_t mac[6])
{
	netbuf_t *packet = netbuf_alloc();

	if (packet == 0) return false;

	uint8_t *message = netbuf_append(packet, 28);
	memset(message, 0, 28);
	net_write16(message, 1);
	net_write16(message + 2, ETHERTYPE_IPV4);
	message[4] = 6;
	message[5] = 4;
	net_write16(message + 6, operation);
	memcpy(message + 8, interface->info.mac, 6);
	memcpy(message + 14, interface->info.address4, 4);
	memcpy(message + 24, address, 4);

	const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};

	if (operation == 2) {
		memcpy(message + 18, mac, 6);
	}

	return ethernet_output(interface, packet, operation == 2 ? mac : broadcast, ETHERTYPE_ARP);
}

bool
arp_resolve(netif_t *interface, const uint8_t address[4], uint8_t mac[6])
{
	arp_entry_t *entry = arp_entry(interface, address);
	uint64_t now = net_now();

	if (entry->valid && now - entry->updated < ARP_LIFETIME_US) {
		memcpy(mac, entry->mac, 6);
		return true;
	}

	if (entry->requested == 0 || now - entry->requested >= ARP_RETRY_US) {
		entry->requested = now;
		(void)arp_send(interface, 1, address, 0);
	}

	return false;
}

void
arp_input(netbuf_t *packet)
{
	uint8_t *message = netbuf_data(packet);
	netif_t *interface = packet->interface;

	if (packet->length < 28 || net_read16(message) != 1 || net_read16(message + 2) != ETHERTYPE_IPV4 ||
		message[4] != 6 || message[5] != 4 || (message[8] & 1) ||
		memcmp(message + 24, interface->info.address4, 4) != 0) {
		netbuf_free(packet);
		return;
	}

	uint16_t operation = net_read16(message + 6);

	if ((operation == 1 || operation == 2) && net_read32(message + 14) != 0) {
		arp_entry_t *entry = arp_entry(interface, message + 14);
		memcpy(entry->mac, message + 8, 6);
		entry->valid = true;
		entry->updated = net_now();
	}

	if (operation == 1) {
		(void)arp_send(interface, 2, message + 14, message + 8);
	}

	netbuf_free(packet);
}
