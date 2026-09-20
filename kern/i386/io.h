/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/io.h
 *
 * x86 I/O-port accessors, shared by the i386 and x86_64 builds.
 */

#ifndef NXU_KERN_I386_IO_H
#define NXU_KERN_I386_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value)
{
	__asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
	uint8_t value;

	__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
	return value;
}

static inline void io_wait(void)
{
	outb(0x80, 0U);
}

#endif
