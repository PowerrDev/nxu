/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/netinet/ip.c
 *
 * Implements IPv4 packet input, output, and forwarding decisions.
 */
#include <kern/net/protocol.h>

void
ip4_input(netbuf_t *packet)
{
	uint8_t *header = netbuf_data(packet);
	netif_t *interface = packet->interface;

	if (packet->length < 20 || (header[0] >> 4) != 4) {
		goto drop;
	}

	uint32_t header_length = (header[0] & 15) * 4;
	uint32_t total = net_read16(header + 2);

	if (header_length < 20 || header_length > packet->length || total < header_length || total > packet->length ||
		net_checksum(header, header_length, 0) != 0 || header[8] == 0 || (net_read16(header + 6) & 0xbfff) != 0) {
		goto drop;
	}

	/* Host-only milestone: reject fragments, broadcast, multicast, and forwarded traffic. */
	bool local = memcmp(header + 16, interface->info.address4, 4) == 0;

	if (interface->info.flags & NXU_NET_IF_LOOPBACK) {
		local = header[16] == 127;
	}

	if (!local || header[16] == 0 || header[12] == 0 || header[12] >= 224) {
		goto drop;
	}

	uint8_t source[4];
	memcpy(source, header + 12, 4);
	uint8_t ttl = header[8];
	uint8_t protocol = header[9];
	packet->length = total;
	(void)netbuf_pull(packet, header_length);

	if (protocol == IPPROTO_ICMP) {
		icmp4_input(packet, source, ttl);
		return;
	}

drop:
	__atomic_fetch_add(&interface->info.dropped, 1, __ATOMIC_RELAXED);
	netbuf_free(packet);
}

bool
ip4_output(netif_t *interface, netbuf_t *packet, const uint8_t destination[4], uint8_t protocol)
{
	uint8_t mac[6];

	if (!(interface->info.flags & NXU_NET_IF_LOOPBACK) && memcmp(destination, interface->info.address4, 4) == 0) {
		memcpy(mac, interface->info.mac, 6);
	} else if (!(interface->info.flags & NXU_NET_IF_LOOPBACK)) {
		uint32_t mask = interface->info.prefix4 == 0 ? 0 : UINT32_MAX << (32 - interface->info.prefix4);
		const uint8_t *neighbor = (net_read32(destination) & mask) == (net_read32(interface->info.address4) & mask)
									  ? destination
									  : interface->info.gateway4;

		if (net_read32(neighbor) == 0 || !arp_resolve(interface, neighbor, mac)) {
			netbuf_free(packet);
			return false;
		}

	}

	uint8_t *header = netbuf_prepend(packet, 20);

	if (header == 0 || packet->length > interface->info.mtu) {
		netbuf_free(packet);
		return false;
	}

	memset(header, 0, 20);
	header[0] = 0x45;
	net_write16(header + 2, packet->length);
	net_write16(header + 6, 0x4000);
	header[8] = 64;
	header[9] = protocol;
	memcpy(header + 12, interface->info.address4, 4);
	memcpy(header + 16, destination, 4);
	net_write16(header + 10, net_checksum(header, 20, 0));

	if (interface->info.flags & NXU_NET_IF_LOOPBACK) return netif_output(interface, packet);

	return ethernet_output(interface, packet, mac, ETHERTYPE_IPV4);
}
