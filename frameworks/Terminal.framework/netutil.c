/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include "netutil.h"

#include <nxu/string.h>
#include <nxu/syscall.h>

void
netutil_write(const char *text)
{
	(void)nxu_write(1, text, nxu_strlen(text));
}

void
netutil_number(uint64_t number)
{
	char buffer[24];
	uint32_t length = 0;

	do {
		buffer[length++] = '0' + number % 10;
		number /= 10;
	} while (number != 0);

	while (length != 0) {
		(void)nxu_write(1, &buffer[--length], 1);
	}
}

static bool
netutil_decimal(const char *text, uint32_t *number)
{
	uint32_t value = 0;

	if (*text == 0) {
		return false;
	}

	while (*text != 0) {
		if (*text < '0' || *text > '9' || value > (UINT32_MAX - (*text - '0')) / 10) {
			return false;
		}

		value = value * 10 + *text++ - '0';
	}

	*number = value;
	return true;
}

bool
netutil_address(const char *text, uint32_t family, uint8_t address[16])
{
	memset(address, 0, 16);

	if (family == NXU_NET_AF_INET) {
		for (uint32_t group = 0; group < 4; group++) {
			uint32_t value = 0;
			uint32_t digits = 0;

			while (*text >= '0' && *text <= '9') {
				value = value * 10 + *text++ - '0';
				if (++digits > 3 || value > 255) {
					return false;
				}
			}

			if (digits == 0 || (group < 3 ? *text++ != '.' : *text != 0)) {
				return false;
			}

			address[group] = value;
		}

		return true;
	}

	if (family != NXU_NET_AF_INET6 || *text == 0) {
		return false;
	}

	uint16_t groups[8];
	uint32_t count = 0;
	int32_t compression = -1;

	if (*text == ':') {
		if (text[1] != ':') {
			return false;
		}

		compression = 0;
		text += 2;
	}

	while (*text != 0) {
		uint32_t value = 0;
		uint32_t digits = 0;

		while (*text != 0 && *text != ':') {
			uint32_t digit;

			if (*text >= '0' && *text <= '9') {
				digit = *text - '0';
			} else if (*text >= 'a' && *text <= 'f') {
				digit = *text - 'a' + 10;
			} else if (*text >= 'A' && *text <= 'F') {
				digit = *text - 'A' + 10;
			} else {
				return false;
			}

			if (++digits > 4) {
				return false;
			}

			value = value * 16 + digit;
			text++;
		}

		if (digits == 0 || count == 8) {
			return false;
		}

		groups[count++] = value;

		if (*text == ':') {
			text++;
			if (*text == ':') {
				if (compression >= 0) {
					return false;
				}

				compression = count;
				text++;
			} else if (*text == 0) {
				return false;
			}
		}
	}

	if ((compression < 0 && count != 8) || (compression >= 0 && count >= 8)) {
		return false;
	}

	uint32_t gap = 8 - count;

	for (uint32_t index = 0; index < count; index++) {
		uint32_t target = compression >= 0 && index >= (uint32_t)compression ? index + gap : index;
		address[target * 2] = groups[index] >> 8;
		address[target * 2 + 1] = groups[index];
	}

	return true;
}

void
netutil_print_address(const uint8_t *address, uint32_t family)
{
	if (family == NXU_NET_AF_INET) {
		for (uint32_t index = 0; index < 4; index++) {
			if (index != 0) {
				netutil_write(".");
			}

			netutil_number(address[index]);
		}

		return;
	}

	static const char hex[] = "0123456789abcdef";

	for (uint32_t index = 0; index < 16; index += 2) {
		if (index != 0) {
			netutil_write(":");
		}

		uint16_t group = ((uint16_t)address[index] << 8) | address[index + 1];
		bool started = false;

		for (int32_t shift = 12; shift >= 0; shift -= 4) {
			uint32_t digit = (group >> shift) & 15;
			if (digit != 0 || started || shift == 0) {
				(void)nxu_write(1, &hex[digit], 1);
				started = true;
			}
		}
	}
}

int
netutil_interfaces(void)
{
	for (uint32_t index = 0;; index++) {
		nxu_net_interface_t interface = {.index = index};
		int64_t result = nxu_network(NXU_NET_INTERFACE, &interface);

		if (result == -NXU_SYS_E_NOT_FOUND) {
			return 0;
		}

		if (result < 0) {
			netutil_write("ifconfig: interface query failed\n");
			return 1;
		}

		netutil_write(interface.name);
		netutil_write((interface.flags & NXU_NET_IF_UP) ? ": UP mtu " : ": DOWN mtu ");
		netutil_number(interface.mtu);
		netutil_write(" index ");
		netutil_number(interface.index);
		netutil_write("\n  inet ");
		netutil_print_address(interface.address4, NXU_NET_AF_INET);
		netutil_write("/");
		netutil_number(interface.prefix4);
		netutil_write(" gateway ");
		netutil_print_address(interface.gateway4, NXU_NET_AF_INET);
		netutil_write("\n  inet6 ");
		netutil_print_address(interface.address6, NXU_NET_AF_INET6);
		netutil_write("\n  link-local ");
		netutil_print_address(interface.link_local6, NXU_NET_AF_INET6);

		if (interface.flags & NXU_NET_IF_DUPLICATE) {
			netutil_write(" DUPLICATE");
		} else if (interface.flags & NXU_NET_IF_TENTATIVE) {
			netutil_write(" tentative");
		}

		netutil_write("\n  RX ");
		netutil_number(interface.received);
		netutil_write(" TX ");
		netutil_number(interface.transmitted);
		netutil_write(" dropped ");
		netutil_number(interface.dropped);
		netutil_write("\n");
	}
}

int
netutil_ping(uint64_t argc, const char *const *argv)
{
	uint32_t count = 4;
	nxu_net_ping_t request = {.timeout_ms = 3000};
	const char *target = 0;

	for (uint64_t index = 1; index < argc; index++) {
		const char *argument = argv[index];

		if (nxu_streq(argument, "-4") || nxu_streq(argument, "-6")) {
			request.family = argument[1] == '4' ? NXU_NET_AF_INET : NXU_NET_AF_INET6;
		} else if (nxu_streq(argument, "-c") || nxu_streq(argument, "-W") || nxu_streq(argument, "-I")) {
			uint32_t number;

			if (++index == argc || !netutil_decimal(argv[index], &number)) {
				goto usage;
			}

			if (argument[1] == 'c') {
				count = number;
			} else if (argument[1] == 'W') {
				request.timeout_ms = number;
			} else {
				request.interface_index = number;
			}
		} else if (target == 0 && argument[0] != '-') {
			target = argument;
		} else {
			goto usage;
		}
	}

	if (target == 0 || count == 0 || count > 1000 || request.timeout_ms == 0 || request.timeout_ms > 5000) {
		goto usage;
	}

	if (request.family == 0) {
		request.family = NXU_NET_AF_INET;
		for (const char *cursor = target; *cursor != 0; cursor++) {
			if (*cursor == ':') {
				request.family = NXU_NET_AF_INET6;
			}
		}
	}

	if (!netutil_address(target, request.family, request.destination)) {
		netutil_write("ping: expected a numeric IPv4 or IPv6 address\n");
		return 2;
	}

	netutil_write("PING ");
	netutil_write(target);
	netutil_write(" (32 data bytes)\n");
	uint32_t received = 0;

	for (uint32_t sequence = 0; sequence < count; sequence++) {
		request.sequence = sequence;
		int64_t result = nxu_network(NXU_NET_PING, &request);

		if (result == 0) {
			received++;
			netutil_number(request.bytes);
			netutil_write(" bytes from ");
			netutil_write(target);
			netutil_write(": seq=");
			netutil_number(sequence);
			netutil_write(" ttl=");
			netutil_number(request.hop_limit);
			netutil_write(" time=");
			netutil_number(request.elapsed_us);
			netutil_write(" us\n");
		} else {
			netutil_write(result == -NXU_SYS_E_AGAIN ? "Request timed out" : "Network error");
			netutil_write(" (seq=");
			netutil_number(sequence);
			netutil_write(")\n");
			if (result != -NXU_SYS_E_AGAIN) {
				return 1;
			}
		}

		if (sequence + 1 < count) {
			(void)nxu_sleep_us(1000000);
		}
	}

	netutil_number(count);
	netutil_write(" transmitted, ");
	netutil_number(received);
	netutil_write(" received\n");
	return received == count ? 0 : 1;

usage:
	netutil_write("usage: ping [-4|-6] [-c count] [-W timeout_ms] [-I interface_index] address\n");
	return 2;
}
