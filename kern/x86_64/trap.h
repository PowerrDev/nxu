/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/trap.h
 *
 * Long-mode trap vectors and the normalized x86-64 exception frame.
 */
#ifndef NXU_KERN_X86_64_TRAP_H
#define NXU_KERN_X86_64_TRAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define X86_IDT_VECTOR_COUNT 256U
#define X86_EXCEPTION_COUNT 32U
#define X86_TRAP_BREAKPOINT 3U
#define X86_TRAP_OVERFLOW 4U
#define X86_TRAP_DOUBLE_FAULT 8U
#define X86_TRAP_PAGE_FAULT 14U
#define X86_TRAP_MACHINE_CHECK 18U
#define X86_TRAP_SPURIOUS 255U

#define X86_PF_PRESENT (1ULL << 0U)
#define X86_PF_WRITE (1ULL << 1U)
#define X86_PF_USER (1ULL << 2U)
#define X86_PF_RESERVED (1ULL << 3U)
#define X86_PF_INSTRUCTION (1ULL << 4U)
#define X86_PF_PROTECTION_KEY (1ULL << 5U)
#define X86_PF_SHADOW_STACK (1ULL << 6U)
#define X86_PF_SGX (1ULL << 15U)

typedef struct {
	uint64_t r15;
	uint64_t r14;
	uint64_t r13;
	uint64_t r12;
	uint64_t r11;
	uint64_t r10;
	uint64_t r9;
	uint64_t r8;
	uint64_t rdi;
	uint64_t rsi;
	uint64_t rbp;
	uint64_t rdx;
	uint64_t rcx;
	uint64_t rbx;
	uint64_t rax;
	uint64_t vector;
	uint64_t error_code;
	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
	uint64_t rsp;
	uint64_t ss;
} x86_trap_frame_t;

_Static_assert(offsetof(x86_trap_frame_t, vector) == 120U, "x86 trap vector offset changed");
_Static_assert(offsetof(x86_trap_frame_t, rip) == 136U, "x86 trap RIP offset changed");
_Static_assert(sizeof(x86_trap_frame_t) == 176U, "x86 trap frame size changed");

extern const int64_t x86_isr_table[X86_IDT_VECTOR_COUNT];

bool x86_idt_initialize(void);
bool x86_trap_self_test(void);
void x86_trap_handler(x86_trap_frame_t *frame);

#endif
