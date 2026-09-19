/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        libk/udivmoddi4.c
 *
 * 64-bit integer division for 32-bit targets. The compiler lowers a 64-bit
 * `/` or `%` on i386 to calls to these routines, and there is no libgcc or
 * compiler-rt in a freestanding kernel to provide them. Plain shift-subtract
 * long division: slow, but tiny, obviously correct and free of any further
 * runtime dependency. Unused on LP64 targets, where the hardware divides.
 */

#include <stdint.h>

#if !defined(__LP64__)

uint64_t __udivmoddi4(uint64_t numerator, uint64_t denominator, uint64_t *remainder)
{
	if (denominator == 0ULL) {
		__asm__ volatile("ud2");
		__builtin_unreachable();
	}

	uint64_t quotient = 0ULL;
	uint64_t partial = 0ULL;

	for (int bit = 63; bit >= 0; bit--) {
		partial = (partial << 1U) | ((numerator >> (uint32_t)bit) & 1ULL);

		if (partial >= denominator) {
			partial -= denominator;
			quotient |= 1ULL << (uint32_t)bit;
		}
	}

	if (remainder != 0) *remainder = partial;
	return quotient;
}

uint64_t __udivdi3(uint64_t numerator, uint64_t denominator)
{
	return __udivmoddi4(numerator, denominator, 0);
}

uint64_t __umoddi3(uint64_t numerator, uint64_t denominator)
{
	uint64_t remainder;

	(void)__udivmoddi4(numerator, denominator, &remainder);
	return remainder;
}

int64_t __divdi3(int64_t numerator, int64_t denominator)
{
	uint64_t n = numerator < 0 ? -(uint64_t)numerator : (uint64_t)numerator;
	uint64_t d = denominator < 0 ? -(uint64_t)denominator : (uint64_t)denominator;
	uint64_t q = __udivmoddi4(n, d, 0);

	return (numerator < 0) != (denominator < 0) ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t numerator, int64_t denominator)
{
	uint64_t n = numerator < 0 ? -(uint64_t)numerator : (uint64_t)numerator;
	uint64_t d = denominator < 0 ? -(uint64_t)denominator : (uint64_t)denominator;
	uint64_t r;

	(void)__udivmoddi4(n, d, &r);

	/* The remainder takes the sign of the numerator, as in C. */
	return numerator < 0 ? -(int64_t)r : (int64_t)r;
}

#endif
