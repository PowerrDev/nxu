/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/net/route.c
 *
 * Maintains routes used to select network interfaces for packets.
 */
#include <kern/net/protocol.h>

netif_t *
net_route(uint32_t family, const uint8_t *destination, uint32_t interface_index)
{
	netif_t *fallback = 0;

	for (uint32_t index = 0; index < netif_count(); index++) {
		netif_t *interface = netif_at(index);

		if (!(interface->info.flags & NXU_NET_IF_UP) ||
			(interface_index != 0 && interface->info.index != interface_index)) {
			continue;
		}

		if (family == NXU_NET_AF_INET) {
			if (memcmp(destination, interface->info.address4, 4) == 0 ||
				((interface->info.flags & NXU_NET_IF_LOOPBACK) && destination[0] == 127)) {
				return interface;
			}

			uint32_t prefix = interface->info.prefix4;
			uint32_t mask = prefix == 0 ? 0 : UINT32_MAX << (32 - prefix);

			if (prefix != 0 && (net_read32(destination) & mask) == (net_read32(interface->info.address4) & mask)) {
				return interface;
			}

			if (net_read32(interface->info.gateway4) != 0) {
				fallback = interface;
			}
		} else if (family == NXU_NET_AF_INET6) {
			if (memcmp(destination, netif_at(0)->info.address6, 16) == 0) return netif_at(0);

			if (!(interface->info.flags & NXU_NET_IF_LOOPBACK)) {
				if (destination[0] == 0xfe && (destination[1] & 0xc0) == 0x80) return interface;

				if (interface->info.address6[0] != 0) {
					fallback = interface;
					if (memcmp(destination, interface->info.address6, 8) == 0) return interface;
				}
			}
		}
	}

	return fallback;
}
