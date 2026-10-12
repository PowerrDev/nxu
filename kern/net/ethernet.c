/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <kern/net/protocol.h>

void
ethernet_input(netbuf_t *packet)
{
	netif_t *interface = packet->interface;
	uint8_t *frame = netbuf_data(packet);
	uint16_t type;

	if (interface->info.flags & NXU_NET_IF_LOOPBACK) {
		if (packet->length == 0) {
			netbuf_free(packet);
			return;
		}

		type = (frame[0] >> 4) == 4 ? ETHERTYPE_IPV4 : ETHERTYPE_IPV6;
	} else {
		if (packet->length < 14 || ((frame[0] & 1) == 0 && memcmp(frame, interface->info.mac, 6) != 0)) {
			__atomic_fetch_add(&interface->info.dropped, 1, __ATOMIC_RELAXED);
			netbuf_free(packet);
			return;
		}

		type = net_read16(frame + 12);
		(void)netbuf_pull(packet, 14);
	}

	switch (type) {
	case ETHERTYPE_ARP:
		arp_input(packet);
		break;

	case ETHERTYPE_IPV4:
		ip4_input(packet);
		break;

	case ETHERTYPE_IPV6:
		ip6_input(packet);
		break;

	default:
		netbuf_free(packet);
		break;
	}
}

bool
ethernet_output(netif_t *interface, netbuf_t *packet, const uint8_t mac[6], uint16_t type)
{
	uint8_t *header = netbuf_prepend(packet, 14);

	if (header == 0 || packet->length > interface->info.mtu + 14) {
		netbuf_free(packet);
		return false;
	}

	memcpy(header, mac, 6);
	memcpy(header + 6, interface->info.mac, 6);
	net_write16(header + 12, type);

	return netif_output(interface, packet);
}
