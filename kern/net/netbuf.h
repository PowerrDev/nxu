/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/net/netbuf.h
 *
 * Declares the kernel net netbuf interfaces and data structures.
 */
#ifndef NXU_KERN_NET_NETBUF_H
#define NXU_KERN_NET_NETBUF_H

#include <stdbool.h>
#include <stdint.h>

#define NETBUF_CAPACITY 2048U
#define NETBUF_COUNT 64U
#define NETBUF_HEADROOM 128U

struct netif;

typedef struct netbuf {
	struct netbuf *next;
	struct netif *interface;
	uint32_t offset;
	uint32_t length;
	bool allocated;
	uint8_t storage[NETBUF_CAPACITY];
} netbuf_t;

/* Alloc returns exclusive ownership. Free consumes it. No sharing or chaining yet. */
netbuf_t *netbuf_alloc(void);
void netbuf_free(netbuf_t *packet);
uint8_t *netbuf_data(netbuf_t *packet);
void *netbuf_append(netbuf_t *packet, uint32_t length);
void *netbuf_prepend(netbuf_t *packet, uint32_t length);
bool netbuf_pull(netbuf_t *packet, uint32_t length);

#endif
