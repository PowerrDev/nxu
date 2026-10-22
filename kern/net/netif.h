/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/net/netif.h
 *
 * Declares the kernel net netif interfaces and data structures.
 */
#ifndef NXU_KERN_NET_NETIF_H
#define NXU_KERN_NET_NETIF_H

#include <kern/net/netbuf.h>
#include <kern/syscall/net_defs.h>

#define NETIF_MAX 4U
#define NET_RX_BUDGET 32U

typedef struct netif {
	nxu_net_interface_t info;
	void *context;
	/* Transmit borrows frame bytes until return. The driver copies them into DMA storage. */
	bool (*transmit)(void *context, const void *frame, uint32_t length);
	void (*poll)(void *context);
	uint64_t dad_deadline;
	uint32_t dad_phase;
} netif_t;

/* Registration is boot-only, before net_start; interface storage is permanent. */
netif_t *netif_register(const uint8_t mac[6], void *context, bool (*transmit)(void *, const void *, uint32_t),
						void (*poll)(void *));
netif_t *netif_at(uint32_t index);
uint32_t netif_count(void);
/* Input consumes packet on both success and failure. Safe in interrupt context. */
void netif_input(netif_t *interface, netbuf_t *packet);
/* Output consumes packet on both success and failure. Stack-worker context only. */
bool netif_output(netif_t *interface, netbuf_t *packet);
netbuf_t *netif_dequeue(void);
void netif_initialize(void);

bool net_start(void);
void net_poll(void);
int64_t net_interface_info(nxu_net_interface_t *info);
int64_t net_ping(nxu_net_ping_t *request);

#endif
