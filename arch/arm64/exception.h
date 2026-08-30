#ifndef NXU_EXCEPTION_H
#define NXU_EXCEPTION_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	ARM64_EXCEPTION_KIND_SYNCHRONOUS = 0,
	ARM64_EXCEPTION_KIND_IRQ = 1,
	ARM64_EXCEPTION_KIND_FIQ = 2,
	ARM64_EXCEPTION_KIND_SERROR = 3,
	ARM64_EXCEPTION_KIND_INVALID = 0xFF
} arm64_exception_kind_t;

typedef enum {
	ARM64_EXCEPTION_ORIGIN_CURRENT_EL_SP0 = 0,
	ARM64_EXCEPTION_ORIGIN_CURRENT_EL_SPX = 1,
	ARM64_EXCEPTION_ORIGIN_LOWER_EL_AARCH64 = 2,
	ARM64_EXCEPTION_ORIGIN_LOWER_EL_AARCH32 = 3,
	ARM64_EXCEPTION_ORIGIN_INVALID = 0xFF
} arm64_exception_origin_t;

/**
 * struct arm64_exception_frame_t - Register state saved on exception entry.
 * @x: General-purpose registers x0 through x30.
 * @sp: Active exception-level stack pointer before the frame was carved.
 * @elr: ELR_EL1, the address execution resumes at.
 * @spsr: SPSR_EL1, the PSTATE value from before the exception.
 * @far: FAR_EL1, the faulting virtual address. Meaningful for aborts only.
 * @esr: ESR_EL1, the exception syndrome.
 * @vector_id: Which of the 16 vector table entries was taken, 0 through 15.
 * @sp_el0: SP_EL0 captured on entry. This is the user stack for EL0 traps.
 *
 * The layout is duplicated as byte offsets in exception_vectors.S and the two
 * must be kept in agreement. The total size is 304 bytes, which preserves the
 * 16-byte stack alignment required by AArch64.
 *
 * For exceptions from EL0, @sp is the kernel exception stack in SP_EL1 while
 * @sp_el0 is the interrupted user stack. For current-EL exceptions, @sp is the
 * interrupted kernel stack and @sp_el0 is preserved for a later EL0 return.
 *
 * The assembly exit path writes @elr and @spsr back to ELR_EL1 and SPSR_EL1.
 * For current-EL SPx and lower-EL entries it also restores @sp_el0 to SP_EL0.
 * A recoverable handler may therefore update those fields to control where
 * and how execution resumes.
 */
typedef struct {
	uint64_t x[31];

	uint64_t sp;
	uint64_t elr;
	uint64_t spsr;
	uint64_t far;
	uint64_t esr;
	uint64_t vector_id;
	uint64_t sp_el0;
} arm64_exception_frame_t;

/**
 * exception_init - Install the EL1 exception vector table.
 *
 * Writes VBAR_EL1 with the address of the vector table and issues an ISB.
 * The table is placed at a 2048-byte boundary by the linker script, as
 * VBAR_EL1 requires.
 */
void exception_init(void);

/** Return the architectural exception kind encoded by a vector-table slot. */
arm64_exception_kind_t exception_vector_kind(uint64_t vector_id);

/** Return the architectural source group encoded by a vector-table slot. */
arm64_exception_origin_t exception_vector_origin(uint64_t vector_id);

/** True only for an exception taken from AArch64 EL0 into EL1. */
bool exception_vector_from_aarch64_el0(uint64_t vector_id);

/** Validate the vector-slot classification used by the EL0 entry path. */
bool exception_validate_vector_classification(void);

/**
 * exception_handle - Dispatch an exception from the assembly entry path.
 * @frame: The saved register frame, allocated on the kernel exception stack.
 *
 * IRQ entries are dispatched to the interrupt controller and return through
 * the common assembly exit path. Other exceptions remain fatal until a
 * dedicated recoverable handler, such as the Lesson 18D SVC path, accepts
 * them and returns.
 */
void exception_handle(arm64_exception_frame_t *frame);

#endif
