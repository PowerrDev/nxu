/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/idt.h
 *
 * Interrupt descriptor table for 32-bit x86.
 *
 *   vectors 0-31    processor exceptions
 *   vectors 32-47   hardware IRQs (PIC, once it is programmed)
 *   vector  0x80    system call gate, callable from ring 3
 *
 * Every other vector is left zeroed, so a stray `int n` raises #GP whose
 * error code names the vector (IDT-flagged selector) instead of running
 * arbitrary code.
 */

#ifndef NXU_MACH_I386_IDT_H
#define NXU_MACH_I386_IDT_H

#include <stdint.h>

#if !defined(__i386__)
#error "mach/i386/idt.h: the x86_64 IDT layout is not implemented yet"
#endif

#define IDT_ENTRY_COUNT 256U

/* Gate type/attribute bytes: present, DPL, gate type. */
#define IDT_GATE_INTERRUPT 0x8EU
#define IDT_GATE_INTERRUPT_USER 0xEEU
#define IDT_GATE_TASK 0x85U

void i386_idt_init(void);

void i386_idt_set_gate(uint8_t vector, uint32_t offset, uint16_t selector, uint8_t attributes);

#endif
