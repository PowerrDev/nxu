/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/i386/portio.h
 *
 * The 16- and 32-bit I/O-port accessors that legacy PCI configuration and
 * VirtIO-PCI need, next to the byte accessors in <kern/i386/io.h>.
 */

#ifndef NXU_PLATFORM_I386_PORTIO_H
#define NXU_PLATFORM_I386_PORTIO_H

#include <kern/i386/io.h>

#include <stdint.h>

static inline void outw(uint16_t port, uint16_t value)
{
	__asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port)
{
	uint16_t value;

	__asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
	return value;
}

static inline void outl(uint16_t port, uint32_t value)
{
	__asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port)
{
	uint32_t value;

	__asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
	return value;
}

#endif
