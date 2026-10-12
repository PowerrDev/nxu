/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 *
 * One bounded protocol worker and a limited echo transaction table. All
 * routes, neighbors, IPv6 address state, and echo transactions use g_lock.
 * Driver DMA locks and the receive-queue lock are released before entry.
 */
#include <kern/net/protocol.h>

#include <kern/console/console.h>
#include <kern/lock.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <kern/syscall/syscall_defs.h>

#define NET_PING_MAX 16U
#define NET_ECHO_LENGTH 40U

typedef struct {
	bool active;
	bool sent;
	bool complete;
	uint16_t identifier;
	uint16_t sequence;
	uint32_t family;
	uint8_t destination[16];
	uint8_t cookie[NET_ECHO_LENGTH - 8];
	uint64_t transmitted;
	uint64_t elapsed;
	uint32_t hops;
	int64_t result;
} net_ping_slot_t;

static nxu_spinlock_t g_lock;
static net_ping_slot_t g_pings[NET_PING_MAX];
static uint16_t g_identifier;
static uint64_t g_cookie;
static bool g_started;

uint64_t
net_now(void)
{
	return timer_get_microseconds();
}

void
net_poll(void)
{
	for (uint32_t index = 0; index < netif_count(); index++) {
		netif_t *interface = netif_at(index);

		if (interface->poll != 0) {
			interface->poll(interface->context);
		}
	}

	uint64_t irq = nxu_spin_lock_irqsave(&g_lock);

	for (uint32_t budget = 0; budget < NET_RX_BUDGET; budget++) {
		netbuf_t *packet = netif_dequeue();

		if (packet == 0) {
			break;
		}

		ethernet_input(packet);
	}

	for (uint32_t index = 1; index < netif_count(); index++) {
		nd6_tick(netif_at(index), net_now());
	}

	nxu_spin_unlock_irqrestore(&g_lock, irq);
}

static void
net_worker(void *context)
{
	(void)context;

	for (;;) {
		net_poll();

		if (!sched_sleep_us(2000)) {
			(void)sched_yield();
		}
	}
}

bool
net_start(void)
{
	if (g_started) {
		return true;
	}

	netif_initialize();
	thread_t thread = 0;
	nxu_cpuset_t affinity;
	cpuset_clear(&affinity);
	cpuset_add(&affinity, 0);

	if (!kernel_thread_create(proc_task(proc_kernel()), net_worker, 0, &thread)) {
		return false;
	}

	if (!thread_set_affinity(thread, &affinity) || !sched_thread_start(thread)) {
		(void)thread_terminate(thread);
		(void)thread_reap(thread);
		thread_deallocate(thread);
		return false;
	}

	thread_deallocate(thread);
	g_started = true;
	kputln("network: IPv4/ICMP and IPv6/ICMPv6 worker started");
	return true;
}

int64_t
net_interface_info(nxu_net_interface_t *info)
{
	uint64_t irq = nxu_spin_lock_irqsave(&g_lock);
	netif_t *interface = netif_at(info->index);

	if (interface != 0) {
		*info = interface->info;
	}

	nxu_spin_unlock_irqrestore(&g_lock, irq);
	return interface == 0 ? -NXU_SYS_E_NOT_FOUND : 0;
}

static bool
net_send_echo(netif_t *interface, net_ping_slot_t *slot)
{
	if (slot->family == NXU_NET_AF_INET6 && (interface->info.flags & (NXU_NET_IF_TENTATIVE | NXU_NET_IF_DUPLICATE))) {
		return false;
	}

	netbuf_t *packet = netbuf_alloc();

	if (packet == 0) {
		return false;
	}

	uint8_t *message = netbuf_append(packet, NET_ECHO_LENGTH);
	memset(message, 0, NET_ECHO_LENGTH);
	message[0] = slot->family == NXU_NET_AF_INET ? 8 : 128;
	net_write16(message + 4, slot->identifier);
	net_write16(message + 6, slot->sequence);
	memcpy(message + 8, slot->cookie, sizeof(slot->cookie));
	slot->transmitted = net_now();

	if (slot->family == NXU_NET_AF_INET) {
		net_write16(message + 2, net_checksum(message, NET_ECHO_LENGTH, 0));
		return ip4_output(interface, packet, slot->destination, IPPROTO_ICMP);
	}

	const uint8_t *source = ip6_source(interface, slot->destination);
	uint32_t seed = ip6_checksum_seed(source, slot->destination, NET_ECHO_LENGTH, IPPROTO_ICMPV6);
	net_write16(message + 2, net_checksum(message, NET_ECHO_LENGTH, seed));
	return ip6_output(interface, packet, source, slot->destination, IPPROTO_ICMPV6, 64);
}

void
net_echo_reply(uint32_t family, const uint8_t *source, const uint8_t *message, uint32_t length, uint8_t hops)
{
	if (length != NET_ECHO_LENGTH) {
		return;
	}

	for (uint32_t index = 0; index < NET_PING_MAX; index++) {
		net_ping_slot_t *slot = &g_pings[index];

		if (slot->active && slot->sent && slot->family == family && slot->identifier == net_read16(message + 4) &&
			slot->sequence == net_read16(message + 6) &&
			memcmp(source, slot->destination, family == NXU_NET_AF_INET ? 4 : 16) == 0 &&
			memcmp(message + 8, slot->cookie, sizeof(slot->cookie)) == 0) {
			slot->complete = true;
			slot->elapsed = net_now() - slot->transmitted;
			slot->hops = hops;
			slot->result = 0;
			return;
		}
	}
}

void
net_echo_error(uint32_t family, const uint8_t *message, uint32_t length, uint32_t error)
{
	(void)error;
	uint32_t header_length;
	const uint8_t *destination;

	if (family == NXU_NET_AF_INET) {
		if (length < 28 || (message[0] >> 4) != 4 || message[9] != IPPROTO_ICMP) {
			return;
		}

		header_length = (message[0] & 15) * 4;
		destination = message + 16;
	} else {
		if (length < 48 || (message[0] >> 4) != 6 || message[6] != IPPROTO_ICMPV6) {
			return;
		}

		header_length = 40;
		destination = message + 24;
	}

	if (header_length < 20 || header_length > length - 8) {
		return;
	}

	const uint8_t *echo = message + header_length;

	if (echo[0] != (family == NXU_NET_AF_INET ? 8 : 128) || echo[1] != 0) {
		return;
	}

	for (uint32_t index = 0; index < NET_PING_MAX; index++) {
		net_ping_slot_t *slot = &g_pings[index];

		if (slot->active && slot->sent && slot->family == family && slot->identifier == net_read16(echo + 4) &&
			slot->sequence == net_read16(echo + 6) &&
			memcmp(destination, slot->destination, family == NXU_NET_AF_INET ? 4 : 16) == 0) {
			slot->complete = true;
			slot->result = -NXU_SYS_E_IO;
			return;
		}
	}
}

int64_t
net_ping(nxu_net_ping_t *request)
{
	if ((request->family != NXU_NET_AF_INET && request->family != NXU_NET_AF_INET6) || request->timeout_ms == 0 ||
		request->timeout_ms > 5000 || request->sequence > UINT16_MAX ||
		(request->family == NXU_NET_AF_INET &&
		 (request->destination[0] == 0 || request->destination[0] >= 224 || request->destination[3] == 255)) ||
		(request->family == NXU_NET_AF_INET6 &&
		 (request->destination[0] == 255 || memcmp(request->destination, (uint8_t[16]){0}, 16) == 0))) {
		return -NXU_SYS_E_INVALID_ARGUMENT;
	}

	uint64_t irq = nxu_spin_lock_irqsave(&g_lock);
	netif_t *interface = net_route(request->family, request->destination, request->interface_index);
	net_ping_slot_t *slot = 0;

	if (interface == 0) {
		nxu_spin_unlock_irqrestore(&g_lock, irq);
		return -NXU_SYS_E_NOT_FOUND;
	}

	for (uint32_t index = 0; index < NET_PING_MAX; index++) {
		if (!g_pings[index].active) {
			slot = &g_pings[index];
			break;
		}
	}

	if (slot == 0) {
		nxu_spin_unlock_irqrestore(&g_lock, irq);
		return -NXU_SYS_E_BUSY;
	}

	memset(slot, 0, sizeof(*slot));
	slot->active = true;
	slot->identifier = ++g_identifier;
	slot->sequence = request->sequence;
	slot->family = request->family;
	memcpy(slot->destination, request->destination, 16);
	uint64_t cookie = ++g_cookie;
	memcpy(slot->cookie, &cookie, sizeof(cookie));
	memcpy(slot->cookie + 8, "NXU kernel echo", 15);
	nxu_spin_unlock_irqrestore(&g_lock, irq);

	uint64_t deadline = net_now() + (uint64_t)request->timeout_ms * 1000;
	int64_t result = -NXU_SYS_E_AGAIN;

	for (;;) {
		irq = nxu_spin_lock_irqsave(&g_lock);

		if (!slot->sent) {
			slot->sent = net_send_echo(interface, slot);
		}

		if (slot->complete) {
			result = slot->result;
			request->elapsed_us = slot->elapsed;
			request->hop_limit = slot->hops;
			request->bytes = NET_ECHO_LENGTH;
		}

		bool done = slot->complete || net_now() >= deadline;

		if (done) {
			slot->active = false;
		}

		nxu_spin_unlock_irqrestore(&g_lock, irq);

		if (done) {
			return result;
		}

		if (!sched_sleep_us(2000)) {
			irq = nxu_spin_lock_irqsave(&g_lock);
			slot->active = false;
			nxu_spin_unlock_irqrestore(&g_lock, irq);
			return -NXU_SYS_E_INTERRUPTED;
		}
	}
}
