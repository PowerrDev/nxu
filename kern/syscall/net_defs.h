/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/syscall/net_defs.h
 *
 * Defines the kernel syscall ABI data structures and constants.
 */
/*
 * Fixed-width network diagnostics ABI shared by both architectures.
 */

#ifndef NXU_KERN_SYSCALL_NET_DEFS_H
#define NXU_KERN_SYSCALL_NET_DEFS_H

#include <stdint.h>

#define NXU_NET_INTERFACE 1U
#define NXU_NET_PING 2U
#define NXU_NET_AF_INET 4U
#define NXU_NET_AF_INET6 6U
#define NXU_NET_IF_UP 1U
#define NXU_NET_IF_LOOPBACK 2U
#define NXU_NET_IF_TENTATIVE 4U
#define NXU_NET_IF_DUPLICATE 8U

/*
 * Addresses use network byte order; no pointers cross this ABI.
 * For NXU_NET_INTERFACE, index is a zero-based enumeration ordinal on input
 * and the interface identifier (usable by ping -I) on output.
 */
typedef struct {
	uint32_t index;
	uint32_t flags;
	uint32_t mtu;
	uint32_t prefix4;
	char name[16];
	uint8_t mac[6];
	uint8_t reserved[2];
	uint8_t address4[4];
	uint8_t gateway4[4];
	uint8_t address6[16];
	uint8_t link_local6[16];
	uint64_t received __attribute__((aligned(8)));
	uint64_t transmitted __attribute__((aligned(8)));
	uint64_t dropped __attribute__((aligned(8)));
} nxu_net_interface_t;

typedef struct {
	uint32_t family;
	uint32_t interface_index; /* zero: select a route */
	uint8_t destination[16];
	uint32_t timeout_ms; /* 1..5000 */
	uint32_t sequence;
	uint64_t elapsed_us;
	uint32_t hop_limit;
	uint32_t bytes;
} nxu_net_ping_t;

_Static_assert(sizeof(nxu_net_interface_t) == 104, "network interface ABI");
_Static_assert(sizeof(nxu_net_ping_t) == 48, "network ping ABI");

#endif
