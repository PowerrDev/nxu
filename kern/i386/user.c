/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/user.c
 *
 * Signal handler frames for 32-bit x86: the machine-dependent half of signal
 * delivery declared in <kern/machine/user.h>.
 *
 * Both routines act on the saved state of the trap the calling thread is
 * inside the kernel for (machine_thread_t::user_frame, which the trap layer
 * sets while a trap from ring 3 is being handled). Editing that frame is all
 * it takes to change where the thread resumes: the trap stub reloads every
 * register from it on the way out.
 *
 * A handler is entered as an ordinary cdecl function call that nobody made:
 *
 *     uesp ->  return address      the restorer (ends in the sigreturn call)
 *              signal              the handler's one argument
 *              magic, mask         for sigreturn
 *              edi .. eax          the interrupted general registers
 *              eip, efl, uesp      and where and how it was running
 *
 * The frame is placed so that the stack is aligned as the i386 System V ABI
 * promises at a function's first instruction (uesp + 4 is a multiple of 16).
 * When the handler returns, its `ret` pops the return address and lands in
 * the restorer with uesp pointing at `signal`; the restorer calls sigreturn,
 * and machine_user_signal_pop finds the frame 4 bytes below uesp.
 *
 * Segment registers are not saved or restored: user code has one flat
 * selector set and cannot be given another. Floating-point state is not
 * saved either, as on arm64.
 */

#include <kern/i386/user.h>

#include <kern/i386/trap.h>
#include <kern/machine/vm_param.h>
#include <vm/user_copy.h>

#include <stdbool.h>
#include <stdint.h>

#define I386_SIGFRAME_MAGIC 0x53494746U /* "SIGF" */

/* The eflags bits user code owns: carry, parity, adjust, zero, sign, direction, overflow. */
#define I386_EFLAGS_USER_MASK 0x00000CD5U
/* What every ring 3 eflags carries besides: the always-one bit and interrupts enabled. */
#define I386_EFLAGS_USER_FIXED 0x00000202U
#define I386_EFLAGS_DIRECTION 0x00000400U

typedef struct {
	uint32_t return_address;
	uint32_t signal;
	uint32_t magic;
	uint32_t mask;
	uint32_t edi;
	uint32_t esi;
	uint32_t ebp;
	uint32_t ebx;
	uint32_t edx;
	uint32_t ecx;
	uint32_t eax;
	uint32_t eip;
	uint32_t efl;
	uint32_t uesp;
} i386_sigframe_t;

/* The frame of the trap from ring 3 the calling thread is handling, or 0. */
static x86_saved_state_t *
machine_user_frame(void)
{
	machine_thread_t *machine = i386_thread_current_machine();

	if (machine == 0 || machine->user_frame == 0 || !x86_saved_state_is_user(machine->user_frame)) return 0;

	return machine->user_frame;
}

bool
machine_user_signal_push(uint64_t handler, uint64_t restorer, uint32_t signal, uint32_t saved_mask)
{
	x86_saved_state_t *state = machine_user_frame();

	if (state == 0 || handler == 0ULL || handler >= VM_MAX_USER_ADDRESS || restorer >= VM_MAX_USER_ADDRESS) return false;

	uint32_t user_sp = state->uesp;

	if (user_sp < sizeof(i386_sigframe_t) + 32U || (uint64_t)user_sp > VM_MAX_USER_ADDRESS) return false;

	/* The highest address at or below the space needed that leaves uesp + 4 a multiple of 16. */
	uint32_t frame_address = ((user_sp - (uint32_t)sizeof(i386_sigframe_t) - 16U) & ~0xFU) + 12U;

	i386_sigframe_t saved = {.return_address = (uint32_t)restorer,
							 .signal = signal,
							 .magic = I386_SIGFRAME_MAGIC,
							 .mask = saved_mask,
							 .edi = state->edi,
							 .esi = state->esi,
							 .ebp = state->ebp,
							 .ebx = state->ebx,
							 .edx = state->edx,
							 .ecx = state->ecx,
							 .eax = state->eax,
							 .eip = state->eip,
							 .efl = state->efl,
							 .uesp = state->uesp};

	/* A stack that cannot hold the frame (overflow, unmapped) fails the
	 * delivery; the caller ends the process as for SIGSEGV. */
	if (!vm_copy_to_user(frame_address, &saved, sizeof(saved))) return false;

	state->uesp = frame_address;
	state->eip = (uint32_t)handler;

	/* A function is entered with the direction flag clear. */
	state->efl = ((state->efl & I386_EFLAGS_USER_MASK) & ~I386_EFLAGS_DIRECTION) | I386_EFLAGS_USER_FIXED;

	return true;
}

bool
machine_user_signal_pop(uint32_t *saved_mask)
{
	x86_saved_state_t *state = machine_user_frame();

	if (state == 0 || saved_mask == 0) return false;

	/* The handler's `ret` consumed the return address; the frame starts one word lower. */
	if (state->uesp < 4U) return false;

	uint32_t frame_address = state->uesp - 4U;

	if ((frame_address & 0xFU) != 12U) return false;

	i386_sigframe_t saved;

	if (!vm_copy_from_user(&saved, frame_address, sizeof(saved))) return false;

	if (saved.magic != I386_SIGFRAME_MAGIC) return false;

	/* The frame is user-writable, so treat every field as untrusted: the
	 * restored context must stay inside user space and at ring 3. */
	if ((uint64_t)saved.eip >= VM_MAX_USER_ADDRESS || (uint64_t)saved.uesp > VM_MAX_USER_ADDRESS) return false;

	state->edi = saved.edi;
	state->esi = saved.esi;
	state->ebp = saved.ebp;
	state->ebx = saved.ebx;
	state->edx = saved.edx;
	state->ecx = saved.ecx;
	state->eax = saved.eax;
	state->eip = saved.eip;
	state->uesp = saved.uesp;

	/* Only the flags user code owns come back; privilege and interrupt bits are the kernel's. */
	state->efl = (saved.efl & I386_EFLAGS_USER_MASK) | I386_EFLAGS_USER_FIXED;

	*saved_mask = saved.mask;

	return true;
}
