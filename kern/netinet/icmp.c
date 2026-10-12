/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <kern/net/protocol.h>

void
icmp4_input(netbuf_t *packet, const uint8_t source[4], uint8_t ttl)
{
	uint8_t *message = netbuf_data(packet);

	if (packet->length < 8 || net_checksum(message, packet->length, 0) != 0) {
		netbuf_free(packet);
		return;
	}

	if (message[0] == 8 && message[1] == 0) {
		message[0] = 0;
		net_write16(message + 2, 0);
		net_write16(message + 2, net_checksum(message, packet->length, 0));
		(void)ip4_output(packet->interface, packet, source, IPPROTO_ICMP);
		return;
	}

	if (message[0] == 0 && message[1] == 0) {
		net_echo_reply(NXU_NET_AF_INET, source, message, packet->length, ttl);
	} else if (message[0] == 3 || message[0] == 11) {
		net_echo_error(NXU_NET_AF_INET, message + 8, packet->length - 8, message[0]);
	}

	netbuf_free(packet);
}
