/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <kern/tests/net_test.h>

#include <kern/console/console.h>
#include <kern/net/netif.h>
#include <string.h>

bool
net_test(void)
{
	const uint8_t addresses[4][16] = {{127, 0, 0, 1},
									  {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
									  {10, 0, 2, 2},
									  {0xfe, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}};

	for (uint32_t index = 0; index < 4; index++) {
		nxu_net_ping_t request = {
			.family = (index & 1) ? NXU_NET_AF_INET6 : NXU_NET_AF_INET, .timeout_ms = 5000, .sequence = index};
		memcpy(request.destination, addresses[index], 16);
		int64_t result = net_ping(&request);

		if (result != 0) {
			kprintf("net_test: case %u failed (%lld)\n", index, (long long)result);
			for (uint32_t device = 0; device < netif_count(); device++) {
				nxu_net_interface_t info = {.index = device};
				(void)net_interface_info(&info);
				kprintf("net_test: %s RX=%llu TX=%llu dropped=%llu flags=%u\n", info.name,
						(unsigned long long)info.received, (unsigned long long)info.transmitted,
						(unsigned long long)info.dropped, info.flags);
			}

			return false;
		}

		kprintf("net_test: case %u echo reply bytes=%u hops=%u time=%llu us\n", index, request.bytes, request.hop_limit,
				(unsigned long long)request.elapsed_us);
	}

	kputln("net_test: IPv4/IPv6 loopback and VirtIO gateway echo passed");
	return true;
}
