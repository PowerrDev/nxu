/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/arm64/user.c
 *
 * See kern/arm64/user.h.
 */

#include <kern/arm64/user.h>

#include <kern/process/thread.h>
#include <kern/machine/vm_param.h>
#include <vm/user_copy.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* "NXUSIGFR". */
#define ARM64_SIGFRAME_MAGIC 0x4E58555349474652ULL

/* PSTATE bits a signal return may restore: N, Z, C, V. Everything else --
 * exception level, interrupt masks -- is forced back to plain EL0. */
#define ARM64_SIGFRAME_SPSR_FLAGS 0xF0000000ULL

typedef struct {
	uint64_t magic;
	uint64_t x[31];
	uint64_t sp;
	uint64_t pc;
	uint64_t spsr;
	uint64_t mask;
} arm64_sigframe_t;

_Static_assert(
	sizeof(arm64_sigframe_t) % 16U == 0U,
	"signal frame must keep the user stack 16-byte aligned"
);

static arm64_exception_frame_t *machine_user_frame(void)
{
	thread_t thread = current_thread();

	return thread == 0 ? 0 : thread->machine.user_frame;
}

bool machine_user_fork_state(machine_thread_t *child)
{
	arm64_exception_frame_t *frame = machine_user_frame();

	if (frame == 0 || child == 0) return false;

	memcpy(child->user.x, frame->x, sizeof(child->user.x));

	/* fork() returns 0 in the child. */
	child->user.x[0] = 0ULL;

	child->user.pc = frame->elr;
	child->user.sp = frame->sp_el0;
	child->user.spsr = frame->spsr & ARM64_SIGFRAME_SPSR_FLAGS;
	child->user.x0 = 0ULL;
	child->user.valid = true;
	child->user.full = true;

	return true;
}

bool machine_user_exec_state(uint64_t entry, uint64_t stack, uint64_t argc, uint64_t argv)
{
	arm64_exception_frame_t *frame = machine_user_frame();
	thread_t thread = current_thread();

	if (frame == 0 || thread == 0 || entry == 0ULL || stack == 0ULL) return false;

	memset(frame->x, 0, sizeof(frame->x));

	frame->x[0] = argc;
	frame->x[1] = argv;
	frame->elr = entry;
	frame->sp_el0 = stack;
	frame->spsr = ARM64_THREAD_SPSR_EL0T;

	/* Keep the thread's recorded initial state in step with what it runs. */
	thread->machine.user.pc = entry;
	thread->machine.user.sp = stack;
	thread->machine.user.spsr = ARM64_THREAD_SPSR_EL0T;
	thread->machine.user.x0 = argc;
	thread->machine.user.full = false;

	return true;
}

bool machine_user_signal_push(uint64_t handler, uint64_t restorer, uint32_t signal, uint32_t saved_mask)
{
	arm64_exception_frame_t *frame = machine_user_frame();

	if (frame == 0 || handler == 0ULL) return false;

	uint64_t user_sp = frame->sp_el0;

	if (user_sp < sizeof(arm64_sigframe_t) || user_sp > VM_MAX_USER_ADDRESS) return false;

	uint64_t frame_address = (user_sp - sizeof(arm64_sigframe_t)) & ~0xFULL;

	arm64_sigframe_t saved;

	saved.magic = ARM64_SIGFRAME_MAGIC;
	memcpy(saved.x, frame->x, sizeof(saved.x));
	saved.sp = frame->sp_el0;
	saved.pc = frame->elr;
	saved.spsr = frame->spsr;
	saved.mask = saved_mask;

	/* A stack that cannot hold the frame (overflow, unmapped) fails the
	 * delivery; the caller kills the process with SIGSEGV. */
	if (!vm_copy_to_user(frame_address, &saved, sizeof(saved))) return false;

	frame->sp_el0 = frame_address;
	frame->elr = handler;
	frame->spsr = ARM64_THREAD_SPSR_EL0T;
	frame->x[0] = signal;
	frame->x[1] = 0ULL;
	frame->x[2] = 0ULL;
	frame->x[30] = restorer;

	return true;
}

bool machine_user_signal_pop(uint32_t *saved_mask)
{
	arm64_exception_frame_t *frame = machine_user_frame();

	if (frame == 0 || saved_mask == 0) return false;

	uint64_t frame_address = frame->sp_el0;

	if ((frame_address & 0xFULL) != 0ULL) return false;

	arm64_sigframe_t saved;

	if (!vm_copy_from_user(&saved, frame_address, sizeof(saved))) return false;
	if (saved.magic != ARM64_SIGFRAME_MAGIC) return false;

	/* The frame is user-writable, so treat every field as untrusted: the
	 * restored context must stay inside user space and at EL0. */
	if (saved.pc >= VM_MAX_USER_ADDRESS || saved.sp > VM_MAX_USER_ADDRESS) return false;

	memcpy(frame->x, saved.x, sizeof(frame->x));
	frame->sp_el0 = saved.sp;
	frame->elr = saved.pc;
	frame->spsr = saved.spsr & ARM64_SIGFRAME_SPSR_FLAGS;

	*saved_mask = (uint32_t)saved.mask;

	return true;
}
