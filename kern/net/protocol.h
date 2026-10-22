/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/net/protocol.h
 *
 * Declares the kernel net protocol interfaces and data structures.
 */
#ifndef NXU_KERN_NET_PROTOCOL_H
#define NXU_KERN_NET_PROTOCOL_H

#include <kern/net/netif.h>
#include <stdint.h>
#include <string.h>

#define ETHERTYPE_IPV4 0x0800U
#define ETHERTYPE_ARP 0x0806U
#define ETHERTYPE_IPV6 0x86ddU
#define IPPROTO_ICMP 1U
#define IPPROTO_ICMPV6 58U

static inline uint16_t
net_read16(const uint8_t *bytes)
{
	return ((uint16_t)bytes[0] << 8) | bytes[1];
}

static inline uint32_t
net_read32(const uint8_t *bytes)
{
	return ((uint32_t)net_read16(bytes) << 16) | net_read16(bytes + 2);
}

static inline void
net_write16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = value >> 8;
	bytes[1] = value;
}

static inline void
net_write32(uint8_t *bytes, uint32_t value)
{
	net_write16(bytes, value >> 16);
	net_write16(bytes + 2, value);
}

uint16_t net_checksum(const void *data, uint32_t length, uint32_t seed);
uint32_t net_checksum_sum(const void *data, uint32_t length, uint32_t seed);
uint32_t ip6_checksum_seed(const uint8_t source[16], const uint8_t destination[16], uint32_t length, uint8_t protocol);

/* Protocol inputs consume packets; outputs consume packets even on failure. */
void ethernet_input(netbuf_t *packet);
bool ethernet_output(netif_t *interface, netbuf_t *packet, const uint8_t mac[6], uint16_t type);
void arp_input(netbuf_t *packet);
bool arp_resolve(netif_t *interface, const uint8_t address[4], uint8_t mac[6]);
void ip4_input(netbuf_t *packet);
bool ip4_output(netif_t *interface, netbuf_t *packet, const uint8_t destination[4], uint8_t protocol);
void icmp4_input(netbuf_t *packet, const uint8_t source[4], uint8_t ttl);
void ip6_input(netbuf_t *packet);
bool ip6_output(netif_t *interface, netbuf_t *packet, const uint8_t source[16], const uint8_t destination[16],
				uint8_t protocol, uint8_t hops);
void icmp6_input(netbuf_t *packet, const uint8_t source[16], const uint8_t destination[16], uint8_t hops);
bool nd6_resolve(netif_t *interface, const uint8_t address[16], uint8_t mac[6]);
void nd6_input(netbuf_t *packet, const uint8_t source[16], const uint8_t destination[16], uint8_t hops);
void nd6_tick(netif_t *interface, uint64_t now);
bool ip6_local(netif_t *interface, const uint8_t address[16]);
const uint8_t *ip6_source(netif_t *interface, const uint8_t destination[16]);
netif_t *net_route(uint32_t family, const uint8_t *destination, uint32_t interface_index);
void net_echo_reply(uint32_t family, const uint8_t *source, const uint8_t *message, uint32_t length, uint8_t hops);
void net_echo_error(uint32_t family, const uint8_t *message, uint32_t length, uint32_t error);
uint64_t net_now(void);

#endif
