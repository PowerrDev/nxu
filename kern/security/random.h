/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/security/random.h
 *
 * The kernel's own source of random bytes: ASLR slides (kern/loader/elf.c)
 * and password salts (kern/security/passcode.c, the setup assistant). It
 * needs nothing outside the machine NXU runs on.
 *
 * A SHA-256 hash DRBG. It is seeded from the CPU's random number instruction
 * when there is one (RDRAND on x86, RNDR on arm64), from the jitter of the
 * cycle counter and from the RTC, and every request mixes in the cycle
 * counter again. Once the system volume is mounted, random_seed_refresh()
 * also mixes in the seed the previous boot left in /var/db/random-seed and
 * leaves a new one, so two boots of the same image do not start alike.
 *
 * Without a CPU instruction, the jitter and the stored seed are all there
 * is: good enough for ASLR and salts, not a hardware-grade generator.
 */

#ifndef NXU_KERN_SECURITY_RANDOM_H
#define NXU_KERN_SECURITY_RANDOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Seed the generator. Safe to call more than once; random_bytes() calls it if nobody has. */
void random_init(void);

/* Fill out with size random bytes. Never fails. */
void random_bytes(void *out, size_t size);

/* Mix caller-supplied bytes (a measurement, a device's noise) into the state. */
void random_add_entropy(const void *data, size_t size);

/*
 * Mix in the seed file on the system volume and replace it. Does nothing
 * (returns false) until the volume is mounted; after the first success it
 * does nothing again for the rest of the boot.
 */
bool random_seed_refresh(void);

#endif
