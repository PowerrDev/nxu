/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/netinet6/nd6.c
 *
 * Implements IPv6 neighbor discovery and address configuration.
 */
#include <kern/net/protocol.h>

#define ND6_CACHE_SIZE 16U
#define ND6_LIFETIME_US 30000000ULL
#define ND6_RETRY_US 250000ULL
#define ND6_DAD_US 1000000ULL

typedef struct {
	netif_t *interface;
	uint8_t address[16];
	uint8_t mac[6];
	uint64_t updated;
	uint64_t requested;
	bool valid;
} nd6_entry_t;

static nd6_entry_t g_neighbors[ND6_CACHE_SIZE];
static uint32_t g_replace;
static const uint8_t g_unspecified[16];

static nd6_entry_t *
nd6_entry(netif_t *interface, const uint8_t address[16], bool create)
{
	for (uint32_t index = 0; index < ND6_CACHE_SIZE; index++) {
		nd6_entry_t *entry = &g_neighbors[index];

		if (entry->interface == interface && memcmp(entry->address, address, 16) == 0) return entry;
	}
	if (!create) return 0;

	nd6_entry_t *entry = &g_neighbors[g_replace++ % ND6_CACHE_SIZE];
	memset(entry, 0, sizeof(*entry));
	entry->interface = interface;
	memcpy(entry->address, address, 16);
	return entry;
}

static bool
nd6_send(netif_t *interface, uint8_t type, const uint8_t source[16], const uint8_t destination[16],
		 const uint8_t target[16], uint32_t flags)
{
	netbuf_t *packet = netbuf_alloc();

	if (packet == 0) return false;

	bool dad = memcmp(source, g_unspecified, 16) == 0;
	uint32_t length = dad ? 24 : 32;
	uint8_t *message = netbuf_append(packet, length);
	memset(message, 0, length);
	message[0] = type;
	net_write32(message + 4, flags);
	memcpy(message + 8, target, 16);

	if (!dad) {
		message[24] = type == 135 ? 1 : 2;
		message[25] = 1;
		memcpy(message + 26, interface->info.mac, 6);
	}

	uint32_t seed = ip6_checksum_seed(source, destination, length, IPPROTO_ICMPV6);
	net_write16(message + 2, net_checksum(message, length, seed));
	return ip6_output(interface, packet, source, destination, IPPROTO_ICMPV6, 255);
}

static bool
nd6_solicit(netif_t *interface, const uint8_t target[16], bool dad)
{
	uint8_t multicast[16] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff};
	memcpy(multicast + 13, target + 13, 3);
	const uint8_t *source = dad ? g_unspecified : ip6_source(interface, target);

	return nd6_send(interface, 135, source, multicast, target, 0);
}

void
nd6_tick(netif_t *interface, uint64_t now)
{
	if (!(interface->info.flags & NXU_NET_IF_TENTATIVE) || (interface->info.flags & NXU_NET_IF_DUPLICATE) ||
		now < interface->dad_deadline) {
		return;
	}

	if (interface->dad_phase == 0) {
		if (nd6_solicit(interface, interface->info.link_local6, true)) {
			interface->dad_phase = 1;
			interface->dad_deadline = now + ND6_DAD_US;
		}
	} else if (interface->dad_phase == 1 && interface->info.address6[0] != 0) {
		if (nd6_solicit(interface, interface->info.address6, true)) {
			interface->dad_phase = 2;
			interface->dad_deadline = now + ND6_DAD_US;
		}
	} else {
		interface->info.flags &= ~NXU_NET_IF_TENTATIVE;
	}
}

bool
nd6_resolve(netif_t *interface, const uint8_t address[16], uint8_t mac[6])
{
	nd6_entry_t *entry = nd6_entry(interface, address, true);
	uint64_t now = net_now();

	if (entry->valid && now - entry->updated < ND6_LIFETIME_US) {
		memcpy(mac, entry->mac, 6);
		return true;
	}

	if (entry->requested == 0 || now - entry->requested >= ND6_RETRY_US) {
		entry->requested = now;
		(void)nd6_solicit(interface, address, false);
	}

	return false;
}

void
nd6_input(netbuf_t *packet, const uint8_t source[16], const uint8_t destination[16], uint8_t hops)
{
	uint8_t *message = netbuf_data(packet);
	netif_t *interface = packet->interface;
	bool unspecified = memcmp(source, g_unspecified, 16) == 0;

	if (hops != 255 || message[1] != 0 || packet->length < 24 || message[8] == 0xff ||
		memcmp(message + 8, g_unspecified, 16) == 0) {
		goto done;
	}

	/* Options must make progress and fit completely inside the ICMPv6 message. */
	const uint8_t *source_mac = 0;
	const uint8_t *target_mac = 0;

	for (uint32_t offset = 24; offset < packet->length;) {
		if (packet->length - offset < 2) {
			goto done;
		}

		uint32_t length = (uint32_t)message[offset + 1] * 8;

		if (length == 0 || length > packet->length - offset) {
			goto done;
		}

		if (message[offset] == 1 || message[offset] == 2) {
			if (length != 8 || (message[offset + 2] & 1)) {
				goto done;
			}

			if (message[offset] == 1) {
				source_mac = message + offset + 2;
			} else {
				target_mac = message + offset + 2;
			}
		}

		offset += length;
	}
	if (message[0] == 135) {
		uint8_t solicited[16] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff};
		memcpy(solicited + 13, message + 21, 3);

		if (unspecified && (source_mac != 0 || memcmp(destination, solicited, 16) != 0)) {
			goto done;
		}

		if (!ip6_local(interface, message + 8)) {
			goto done;
		}

		if (interface->info.flags & NXU_NET_IF_TENTATIVE) {
			if (unspecified) {
				interface->info.flags |= NXU_NET_IF_DUPLICATE;
			}
			goto done;
		}

		if (interface->info.flags & NXU_NET_IF_DUPLICATE) {
			goto done;
		}

		if (!unspecified && source_mac != 0) {
			nd6_entry_t *entry = nd6_entry(interface, source, true);
			memcpy(entry->mac, source_mac, 6);
			entry->valid = true;
			entry->updated = net_now();
		}

		const uint8_t all_nodes[16] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
		(void)nd6_send(interface, 136, message + 8, unspecified ? all_nodes : source, message + 8,
					   unspecified ? 0x20000000U : 0x60000000U);
	} else {
		uint32_t flags = net_read32(message + 4);

		if (unspecified || (destination[0] == 0xff && (flags & 0x40000000U))) {
			goto done;
		}

		if (ip6_local(interface, message + 8) && (interface->info.flags & NXU_NET_IF_TENTATIVE)) {
			interface->info.flags |= NXU_NET_IF_DUPLICATE;
			goto done;
		}

		nd6_entry_t *entry = nd6_entry(interface, message + 8, false);

		if (entry != 0 && target_mac != 0 &&
			(!entry->valid || (flags & 0x20000000U) || memcmp(entry->mac, target_mac, 6) == 0)) {
			memcpy(entry->mac, target_mac, 6);
			entry->updated = net_now();
			entry->valid = true;
		}
	}

done:
	netbuf_free(packet);
}
