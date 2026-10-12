/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <kern/net/protocol.h>

bool
ip6_local(netif_t *interface, const uint8_t address[16])
{
	if (memcmp(address, (uint8_t[16]){0}, 16) == 0) {
		return false;
	}

	return memcmp(address, interface->info.address6, 16) == 0 || memcmp(address, interface->info.link_local6, 16) == 0;
}

const uint8_t *
ip6_source(netif_t *interface, const uint8_t destination[16])
{
	if (destination[0] == 0xff || (destination[0] == 0xfe && (destination[1] & 0xc0) == 0x80)) {
		return interface->info.link_local6;
	}

	return interface->info.address6;
}

static bool
ip6_options_valid(const uint8_t *header, uint32_t length)
{
	uint32_t offset = 2;

	while (offset < length) {
		uint8_t type = header[offset++];

		if (type == 0) {
			continue;
		}

		if (offset == length || header[offset] > length - offset - 1) {
			return false;
		}

		/* PadN and unknown options whose action bits permit skipping. */
		if ((type & 0xc0) != 0) {
			return false;
		}

		offset += header[offset] + 1;
	}

	return true;
}

void
ip6_input(netbuf_t *packet)
{
	uint8_t *header = netbuf_data(packet);
	netif_t *interface = packet->interface;

	if (packet->length < 40 || (header[0] >> 4) != 6 || header[7] == 0) {
		goto drop;
	}

	uint32_t payload_length = net_read16(header + 4);

	if (payload_length == 0 || payload_length > packet->length - 40 || header[8] == 0xff) {
		goto drop;
	}

	uint8_t source[16];
	uint8_t destination[16];
	memcpy(source, header + 8, 16);
	memcpy(destination, header + 24, 16);

	const uint8_t all_nodes[16] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
	const uint8_t solicited_prefix[13] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff};
	bool multicast = memcmp(destination, all_nodes, 16) == 0 ||
					 (memcmp(destination, solicited_prefix, 13) == 0 &&
					  (memcmp(destination + 13, interface->info.address6 + 13, 3) == 0 ||
					   memcmp(destination + 13, interface->info.link_local6 + 13, 3) == 0));

	if (!ip6_local(interface, destination) && !multicast) {
		goto drop;
	}

	uint8_t next = header[6];
	uint8_t hops = header[7];
	packet->length = payload_length + 40;
	(void)netbuf_pull(packet, 40);

	for (uint32_t count = 0; next != IPPROTO_ICMPV6 && count < 8; count++) {
		header = netbuf_data(packet);
		uint32_t length;

		if (packet->length < 8) {
			goto drop;
		}

		if (next == 0 || next == 60) {
			length = ((uint32_t)header[1] + 1) * 8;
			if (length > packet->length || !ip6_options_valid(header, length)) {
				goto drop;
			}
		} else if (next == 44 && header[1] == 0 && net_read16(header + 2) == 0) {
			/* Atomic fragments carry a complete packet; reassembly is not enabled. */
			length = 8;
		} else {
			goto drop;
		}

		next = header[0];
		(void)netbuf_pull(packet, length);
	}

	if (next == IPPROTO_ICMPV6) {
		icmp6_input(packet, source, destination, hops);
		return;
	}

drop:
	__atomic_fetch_add(&interface->info.dropped, 1, __ATOMIC_RELAXED);
	netbuf_free(packet);
}

bool
ip6_output(netif_t *interface, netbuf_t *packet, const uint8_t source[16], const uint8_t destination[16],
		   uint8_t protocol, uint8_t hops)
{
	uint8_t mac[6];

	if (!(interface->info.flags & NXU_NET_IF_LOOPBACK)) {
		if (ip6_local(interface, destination)) {
			memcpy(mac, interface->info.mac, 6);
		} else if (destination[0] == 0xff) {
			mac[0] = 0x33;
			mac[1] = 0x33;
			memcpy(mac + 2, destination + 12, 4);
		} else {
			uint8_t gateway[16] = {0xfe, 0xc0};
			gateway[15] = 2;
			bool on_link = memcmp(destination, interface->info.address6, 8) == 0 ||
						   (destination[0] == 0xfe && (destination[1] & 0xc0) == 0x80);

			if (!nd6_resolve(interface, on_link ? destination : gateway, mac)) {
				netbuf_free(packet);
				return false;
			}
		}
	}

	uint8_t *header = netbuf_prepend(packet, 40);

	if (header == 0 || packet->length > interface->info.mtu) {
		netbuf_free(packet);
		return false;
	}

	memset(header, 0, 40);
	header[0] = 0x60;
	net_write16(header + 4, packet->length - 40);
	header[6] = protocol;
	header[7] = hops;
	memcpy(header + 8, source, 16);
	memcpy(header + 24, destination, 16);

	if (interface->info.flags & NXU_NET_IF_LOOPBACK) {
		return netif_output(interface, packet);
	}

	return ethernet_output(interface, packet, mac, ETHERTYPE_IPV6);
}
