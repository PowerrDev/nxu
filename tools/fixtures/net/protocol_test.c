/* Exercise real stack code with a deterministic Ethernet peer and malformed frames. */
#include <kern/net/protocol.h>
#include <kern/syscall/syscall_defs.h>
#include <frameworks/Terminal.framework/netutil.h>
#include <frameworks/Terminal.framework/shell.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t clock_us = 1;
static netif_t *interface;
static const uint8_t peer_mac[6] = {2, 0, 0, 0, 0, 2};
static uint32_t echo_replies;
static bool silent;
static bool corrupt;

uint64_t
timer_get_microseconds(void)
{
	return clock_us;
}

bool
sched_sleep_us(uint64_t microseconds)
{
	clock_us += microseconds;
	net_poll();
	return true;
}

bool
sched_yield(void)
{
	return sched_sleep_us(2000);
}

int64_t
nxu_write(uint64_t descriptor, const void *bytes, uint64_t length)
{
	(void)descriptor;
	(void)bytes;
	return length;
}

int64_t
nxu_network(uint32_t operation, void *argument)
{
	return operation == NXU_NET_PING ? net_ping(argument) : net_interface_info(argument);
}

int64_t
nxu_sleep_us(uint64_t microseconds)
{
	(void)sched_sleep_us(microseconds);
	return 0;
}

static void
inject(const uint8_t *frame, uint32_t length)
{
	netbuf_t *packet = netbuf_alloc();
	assert(packet != 0);
	void *bytes = netbuf_append(packet, length);
	assert(bytes != 0);
	memcpy(bytes, frame, length);
	netif_input(interface, packet);
}

static bool
peer_transmit(void *context, const void *data, uint32_t length)
{
	(void)context;
	const uint8_t *frame = data;
	assert(length >= 14 && length <= 1514);

	if (silent) {
		return true;
	}

	uint8_t reply[1600];
	memcpy(reply, frame, length);
	memcpy(reply, interface->info.mac, 6);
	memcpy(reply + 6, peer_mac, 6);
	uint16_t type = net_read16(frame + 12);

	if (type == ETHERTYPE_ARP) {
		const uint8_t *request = frame + 14;
		assert(length == 42 && net_read16(request + 6) == 1);
		uint8_t *message = reply + 14;
		net_write16(message + 6, 2);
		memcpy(message + 8, peer_mac, 6);
		memcpy(message + 14, request + 24, 4);
		memcpy(message + 18, interface->info.mac, 6);
		memcpy(message + 24, request + 14, 4);
	} else if (type == ETHERTYPE_IPV4) {
		uint8_t *ip = reply + 14;
		assert(net_checksum(ip, 20, 0) == 0);
		assert(net_checksum(ip + 20, length - 34, 0) == 0);

		if (ip[20] == 0) {
			echo_replies++;
			return true;
		}

		assert(ip[20] == 8);
		memcpy(ip + 12, frame + 30, 4);
		memcpy(ip + 16, frame + 26, 4);
		ip[20] = 0;
		net_write16(ip + 22, 0);
		if (corrupt) {
			ip[28] ^= 1;
		}
		net_write16(ip + 22, net_checksum(ip + 20, length - 34, 0));
		net_write16(ip + 10, 0);
		net_write16(ip + 10, net_checksum(ip, 20, 0));
	} else if (type == ETHERTYPE_IPV6) {
		const uint8_t *out_ip = frame + 14;
		uint8_t *ip = reply + 14;
		const uint8_t *message = out_ip + 40;
		assert(net_checksum(message, length - 54, ip6_checksum_seed(out_ip + 8, out_ip + 24, length - 54, 58)) == 0);

		if (message[0] == 135) {
			if (memcmp(out_ip + 8, (uint8_t[16]){0}, 16) == 0) {
				return true; /* DAD: no peer owns these addresses. */
			}

			memset(ip, 0, 72);
			ip[0] = 0x60;
			net_write16(ip + 4, 32);
			ip[6] = 58;
			ip[7] = 255;
			memcpy(ip + 8, message + 8, 16);
			memcpy(ip + 24, out_ip + 8, 16);
			ip[40] = 136;
			net_write32(ip + 44, 0x60000000);
			memcpy(ip + 48, message + 8, 16);
			ip[64] = 2;
			ip[65] = 1;
			memcpy(ip + 66, peer_mac, 6);
			length = 86;
		} else if (message[0] == 129) {
			echo_replies++;
			return true;
		} else {
			assert(message[0] == 128);
			memcpy(ip + 8, out_ip + 24, 16);
			memcpy(ip + 24, out_ip + 8, 16);
			ip[40] = 129;
			if (corrupt) {
				ip[48] ^= 1;
			}
		}

		net_write16(ip + 42, 0);
		net_write16(ip + 42, net_checksum(ip + 40, length - 54, ip6_checksum_seed(ip + 8, ip + 24, length - 54, 58)));
	} else {
		assert(false);
	}

	inject(reply, length);
	return true;
}

static void
peer_poll(void *context)
{
	(void)context;
}

static void
test_ping(uint32_t family, const uint8_t *address, int64_t expected)
{
	nxu_net_ping_t request = {.family = family, .timeout_ms = 3000, .sequence = 17};
	memcpy(request.destination, address, family == 4 ? 4 : 16);
	assert(net_ping(&request) == expected);

	if (expected == 0) {
		assert(request.bytes == 40 && request.hop_limit == 64);
	}
}

static void
test_pool(void)
{
	netbuf_t *packets[NETBUF_COUNT];

	for (uint32_t index = 0; index < NETBUF_COUNT; index++) {
		packets[index] = netbuf_alloc();
		assert(packets[index] != 0);
	}

	assert(netbuf_alloc() == 0);
	assert(netbuf_append(packets[0], NETBUF_CAPACITY) == 0);
	assert(netbuf_prepend(packets[0], NETBUF_HEADROOM + 1) == 0);
	assert(!netbuf_pull(packets[0], 1));

	for (uint32_t index = 0; index < NETBUF_COUNT; index++) {
		netbuf_free(packets[index]);
	}
}

static void
test_parsers(void)
{
	uint8_t address[16];
	assert(netutil_address("127.0.0.1", 4, address) && address[0] == 127 && address[3] == 1);
	assert(netutil_address("::1", 6, address) && address[15] == 1);
	assert(netutil_address("fec0::2", 6, address) && address[0] == 0xfe && address[15] == 2);
	assert(netutil_address("1:2:3:4:5:6:7:8", 6, address));
	const char *bad[] = {"1::2::3", ":1", "1:", "1:2", "12345::", "1:2:3:4:5:6:7:8::"};

	for (uint32_t index = 0; index < sizeof(bad) / sizeof(bad[0]); index++) {
		assert(!netutil_address(bad[index], 6, address));
	}

	assert(!netutil_address("256.0.0.1", 4, address));
	assert(!netutil_address("1.2.3", 4, address));
	assert(!netutil_address("1.2.3.4x", 4, address));
	char line[] = "echo 'a b' \"\" c\\ d";
	const char *arguments[NZH_ARGUMENT_MAX + 1];
	assert(nzh_parse(line, arguments) == 4);
	assert(strcmp(arguments[1], "a b") == 0 && strcmp(arguments[2], "") == 0 && strcmp(arguments[3], "c d") == 0);
	char quote[] = "echo 'unterminated";
	char pipe[] = "echo a | cat";
	assert(nzh_parse(quote, arguments) < 0 && nzh_parse(pipe, arguments) < 0);
}

static void
test_incoming_echo(void)
{
	uint8_t frame[86] = {0};
	memcpy(frame, interface->info.mac, 6);
	memcpy(frame + 6, peer_mac, 6);
	net_write16(frame + 12, ETHERTYPE_IPV4);
	uint8_t *ip = frame + 14;
	ip[0] = 0x45;
	net_write16(ip + 2, 32);
	ip[8] = 64;
	ip[9] = 1;
	memcpy(ip + 12, (uint8_t[]){10, 0, 2, 2}, 4);
	memcpy(ip + 16, interface->info.address4, 4);
	ip[20] = 8;
	net_write16(ip + 22, net_checksum(ip + 20, 12, 0));
	net_write16(ip + 10, net_checksum(ip, 20, 0));
	uint32_t replies = echo_replies;
	inject(frame, 46);
	net_poll();
	assert(echo_replies == replies + 1);

	/* A bad IP checksum and a bad ICMP checksum must each stop the reply. */
	ip[10] ^= 1;
	inject(frame, 46);
	net_poll();
	ip[10] ^= 1;
	ip[22] ^= 1;
	inject(frame, 46);
	net_poll();
	assert(echo_replies == replies + 1);

	memset(ip, 0, 72);
	net_write16(frame + 12, ETHERTYPE_IPV6);
	ip[0] = 0x60;
	net_write16(ip + 4, 12);
	ip[6] = 58;
	ip[7] = 64;
	memcpy(ip + 8, (uint8_t[16]){0xfe, 0xc0, [15] = 2}, 16);
	memcpy(ip + 24, interface->info.address6, 16);
	ip[40] = 128;
	net_write16(ip + 42, net_checksum(ip + 40, 12, ip6_checksum_seed(ip + 8, ip + 24, 12, 58)));
	inject(frame, 66);
	net_poll();
	assert(echo_replies == replies + 2);
	ip[42] ^= 1;
	inject(frame, 66);
	net_poll();
	assert(echo_replies == replies + 2);

	/* Valid outer headers cannot make a zero-length ND option safe. */
	memset(ip + 40, 0, 32);
	net_write16(ip + 4, 32);
	ip[7] = 255;
	ip[40] = 135;
	memcpy(ip + 48, interface->info.address6, 16);
	ip[64] = 1;
	ip[65] = 0;
	net_write16(ip + 42, net_checksum(ip + 40, 32, ip6_checksum_seed(ip + 8, ip + 24, 32, 58)));
	inject(frame, 86);
	net_poll();
	assert(echo_replies == replies + 2 && !(interface->info.flags & NXU_NET_IF_DUPLICATE));
}

int
main(void)
{
	test_pool();
	test_parsers();
	const uint8_t checksum_data[] = {0, 1, 0xf2, 3, 0xf4, 0xf5, 0xf6, 0xf7};
	assert(net_checksum(checksum_data, sizeof(checksum_data), 0) == 0x220d);
	assert(net_checksum((uint8_t[]){1, 2, 3}, 3, 0) == 0xfbfd);
	assert(net_start());
	interface = netif_register((uint8_t[]){2, 0, 0, 0, 0, 15}, 0, peer_transmit, peer_poll);
	assert(interface != 0);

	test_ping(4, (uint8_t[]){127, 0, 0, 1}, 0);
	test_ping(6, (uint8_t[16]){[15] = 1}, 0);
	test_ping(4, (uint8_t[]){10, 0, 2, 2}, 0);
	test_ping(6, (uint8_t[16]){0xfe, 0xc0, [15] = 2}, 0);
	assert(!(interface->info.flags & NXU_NET_IF_TENTATIVE));
	test_ping(4, interface->info.address4, 0);
	test_ping(6, interface->info.address6, 0);
	test_incoming_echo();
	corrupt = true;
	test_ping(4, (uint8_t[]){10, 0, 2, 2}, -NXU_SYS_E_AGAIN);
	test_ping(6, (uint8_t[16]){0xfe, 0xc0, [15] = 2}, -NXU_SYS_E_AGAIN);
	corrupt = false;
	silent = true;
	test_ping(4, (uint8_t[]){10, 0, 2, 99}, -NXU_SYS_E_AGAIN);
	silent = false;

	/* Malformed wire input exercises all three dispatch paths under sanitizers. */
	uint32_t random = 0x12345678;
	uint8_t frame[1700];

	for (uint32_t attempt = 0; attempt < 5000; attempt++) {
		for (uint32_t index = 0; index < sizeof(frame); index++) {
			random = random * 1664525U + 1013904223U;
			frame[index] = random >> 24;
		}

		memcpy(frame, interface->info.mac, 6);
		net_write16(frame + 12, (uint16_t[]){ETHERTYPE_ARP, ETHERTYPE_IPV4, ETHERTYPE_IPV6}[attempt % 3]);
		inject(frame, random % sizeof(frame));
		net_poll();
	}

	test_pool();
	puts("PASS bounded buffers, checksums, IPv4/IPv6 loopback, ARP/NDP gateway echo, DAD, reply cookies, timeouts, "
		 "address/shell parsing, 5000 malformed frames");
	return 0;
}
