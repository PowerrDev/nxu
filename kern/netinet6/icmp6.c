/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/netinet6/icmp6.c
 *
 * Implements IPv6 ICMP message handling.
 */
#include <kern/net/protocol.h>

void
icmp6_input(netbuf_t *packet, const uint8_t source[16], const uint8_t destination[16], uint8_t hops)
{
	uint8_t *message = netbuf_data(packet);
	uint32_t seed = ip6_checksum_seed(source, destination, packet->length, IPPROTO_ICMPV6);

	if (packet->length < 8 || net_checksum(message, packet->length, seed) != 0) {
		netbuf_free(packet);
		
		return;
	}

	if (message[0] == 135 || message[0] == 136) {
		nd6_input(packet, source, destination, hops);

		return;
	}

	const uint8_t unspecified[16] = {0};

	if (memcmp(source, unspecified, 16) == 0 || destination[0] == 0xff ||
		(packet->interface->info.flags & (NXU_NET_IF_TENTATIVE | NXU_NET_IF_DUPLICATE))) {
		netbuf_free(packet);
		
		return;
	}

	if (message[0] == 128 && message[1] == 0) {
		message[0] = 129;

		net_write16(message + 2, 0);
		seed = ip6_checksum_seed(destination, source, packet->length, IPPROTO_ICMPV6);
		
		net_write16(message + 2, net_checksum(message, packet->length, seed));
		
		(void)ip6_output(packet->interface, packet, destination, source, IPPROTO_ICMPV6, 64);
		
		return;
	}

	if (message[0] == 129 && message[1] == 0) {
		net_echo_reply(NXU_NET_AF_INET6, source, message, packet->length, hops);
	} else if (message[0] >= 1 && message[0] <= 4) {
		net_echo_error(NXU_NET_AF_INET6, message + 8, packet->length - 8, message[0]);
	}

	netbuf_free(packet);
}
