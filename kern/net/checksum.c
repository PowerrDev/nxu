/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <kern/net/protocol.h>

uint32_t
net_checksum_sum(const void *data, uint32_t length, uint32_t seed)
{
	const uint8_t *bytes = data;

	while (length >= 2) {
		seed += net_read16(bytes);
		bytes += 2;
		length -= 2;
	}

	if (length) {
		seed += (uint32_t)bytes[0] << 8;
	}

	return seed;
}

uint16_t
net_checksum(const void *data, uint32_t length, uint32_t seed)
{
	uint32_t sum = net_checksum_sum(data, length, seed);

	while (sum >> 16) {
		sum = (sum & 0xffffU) + (sum >> 16);
	}

	return (uint16_t)~sum;
}

uint32_t
ip6_checksum_seed(const uint8_t source[16], const uint8_t destination[16], uint32_t length, uint8_t protocol)
{
	uint32_t sum = net_checksum_sum(source, 16, 0);
	sum = net_checksum_sum(destination, 16, sum);
	return sum + (length >> 16) + (length & 0xffffU) + protocol;
}
