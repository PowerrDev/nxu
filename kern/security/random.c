/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/security/random.c
 *
 * See kern/security/random.h.
 *
 * State: a 32-byte key and a 64-bit counter. Output block i of a request is
 * SHA-256("out" || key || counter++ || cycle counter); after each request
 * the key is replaced by SHA-256("next" || key || counter++), so bytes
 * already handed out cannot be recomputed from a later state. Entropy is
 * added by hashing it into the key.
 */

#include <kern/security/random.h>

#include <kern/console/console.h>
#include <kern/lock.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <libk/pbkdf2.h>
#include <libk/sha256.h>
#include <libk/string.h>
#include <platform/rtc.h>
#include <vfs/file.h>
#include <vfs/vfs.h>
#include <vfs/vnode.h>

#define RANDOM_SEED_DIRECTORY "/Volumes/mainOS/var/db"
#define RANDOM_SEED_PATH RANDOM_SEED_DIRECTORY "/random-seed"
#define RANDOM_SEED_SIZE 64U
#define RANDOM_JITTER_SAMPLES 256U

static nxu_spinlock_t g_random_lock = NXU_SPINLOCK_INIT;
static uint8_t g_random_key[SHA256_DIGEST_SIZE];
static uint64_t g_random_counter;
static volatile uint32_t g_random_ready;
static volatile uint32_t g_random_seeded_from_disk;
static bool g_random_hardware;

/* ---- the CPU's own generator -------------------------------------------- */

#if defined(__i386__) || defined(__x86_64__)

#include <kern/machine/cpu.h>

static bool
random_hardware_present(void)
{
	uint32_t registers[4];

	cpu_cpuid(0U, registers);
	if (registers[0] < 1U) return false;
	cpu_cpuid(1U, registers);
	return (registers[2] & (1U << 30)) != 0U; /* CPUID.1:ECX.RDRAND */
}

static bool
random_hardware_word(uint32_t *word)
{
	for (uint32_t attempt = 0U; attempt < 10U; attempt++) {
		uint32_t value;
		uint8_t ok;

		__asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok) : : "cc");
		if (ok) {
			*word = value;
			return true;
		}
	}
	return false;
}

#elif defined(__aarch64__)

static bool
random_hardware_present(void)
{
	uint64_t isar0;

	__asm__ volatile("mrs %0, s3_0_c0_c6_0" : "=r"(isar0)); /* ID_AA64ISAR0_EL1 */
	return ((isar0 >> 60) & 0xFU) != 0U;					 /* RNDR */
}

static bool
random_hardware_word(uint32_t *word)
{
	for (uint32_t attempt = 0U; attempt < 10U; attempt++) {
		uint64_t value;
		uint64_t failed;

		/* RNDR sets NZCV to 0b0100 (Z) when it has no number to give. */
		__asm__ volatile("mrs %0, s3_3_c2_c4_0\n\tcset %1, eq" : "=r"(value), "=r"(failed) : : "cc");
		if (!failed) {
			*word = (uint32_t)(value ^ (value >> 32));
			return true;
		}
	}
	return false;
}

#else

static bool
random_hardware_present(void)
{
	return false;
}

static bool
random_hardware_word(uint32_t *word)
{
	(void)word;
	return false;
}

#endif

/* ---- the DRBG ------------------------------------------------------------ */

/* Hash data into the key. Called with the lock held. */
static void
random_mix_locked(const void *data, size_t size)
{
	sha256_context_t context;
	uint64_t ticks = timer_get_ticks();

	sha256_init(&context);
	sha256_update(&context, "mix", 3U);
	sha256_update(&context, g_random_key, sizeof(g_random_key));
	sha256_update(&context, &ticks, sizeof(ticks));
	if (size != 0U) sha256_update(&context, data, size);
	sha256_final(&context, g_random_key);
	crypto_wipe(&context, sizeof(context));
}

void
random_add_entropy(const void *data, size_t size)
{
	uint64_t state = nxu_spin_lock_irqsave(&g_random_lock);

	random_mix_locked(data, size);
	nxu_spin_unlock_irqrestore(&g_random_lock, state);
}

void
random_init(void)
{
	if (__atomic_load_n(&g_random_ready, __ATOMIC_ACQUIRE) != 0U) return;

	kprintf("initializing the kernel random number generator\n");

	/* Gathered without the lock: the CPU instruction and the counter jitter take a while. */
	uint8_t seed[SHA256_DIGEST_SIZE * 2U + RANDOM_JITTER_SAMPLES / 8U * 8U];
	size_t used = 0U;
	bool hardware = random_hardware_present();
	uint32_t hardware_words = 0U;

	if (hardware) {
		for (uint32_t index = 0U; index < 16U; index++) {
			uint32_t word;

			if (!random_hardware_word(&word)) break;
			memcpy(seed + used, &word, sizeof(word));
			used += sizeof(word);
			hardware_words++;
		}
	}

	/*
	 * The low bits of the time a little work takes: interrupts, caches and
	 * the host's scheduling make them vary. Folded eight samples a byte.
	 */
	uint64_t previous = timer_get_ticks();
	volatile uint32_t sink = 0U;

	for (uint32_t index = 0U; index < RANDOM_JITTER_SAMPLES; index += 8U) {
		uint8_t byte = 0U;

		for (uint32_t bit = 0U; bit < 8U; bit++) {
			for (uint32_t spin = 0U; spin < 64U + (previous & 63U); spin++)
				sink += spin;
			uint64_t now = timer_get_ticks();

			byte = (uint8_t)((byte << 1) | (byte >> 7)) ^ (uint8_t)(now - previous);
			previous = now;
		}
		seed[used++] = byte;
	}

	uint64_t extra[3] = {rtc_unix_time(), timer_get_ticks(), (uint64_t)(uintptr_t)&sink};

	uint64_t state = nxu_spin_lock_irqsave(&g_random_lock);

	random_mix_locked(seed, used);
	random_mix_locked(extra, sizeof(extra));
	g_random_hardware = hardware_words != 0U;
	__atomic_store_n(&g_random_ready, 1U, __ATOMIC_RELEASE);
	nxu_spin_unlock_irqrestore(&g_random_lock, state);
	crypto_wipe(seed, sizeof(seed));

	if (hardware_words != 0U)
		kprintf("seeded from the CPU's random number instruction, the cycle counter and the clock\n");
	else
		kprintf("no CPU random number instruction: seeded from the cycle counter and the clock\n");
	kprintf("initialized the kernel random number generator successfully\n");
}

void
random_bytes(void *out, size_t size)
{
	uint8_t *bytes = (uint8_t *)out;

	if (__atomic_load_n(&g_random_ready, __ATOMIC_ACQUIRE) == 0U) random_init();

	uint32_t word = 0U;
	bool have_word = g_random_hardware && random_hardware_word(&word);
	uint64_t state = nxu_spin_lock_irqsave(&g_random_lock);

	if (have_word) random_mix_locked(&word, sizeof(word));

	while (size != 0U) {
		sha256_context_t context;
		uint8_t block[SHA256_DIGEST_SIZE];
		uint64_t ticks = timer_get_ticks();
		size_t take = size < sizeof(block) ? size : sizeof(block);

		sha256_init(&context);
		sha256_update(&context, "out", 3U);
		sha256_update(&context, g_random_key, sizeof(g_random_key));
		sha256_update(&context, &g_random_counter, sizeof(g_random_counter));
		sha256_update(&context, &ticks, sizeof(ticks));
		sha256_final(&context, block);
		g_random_counter++;
		memcpy(bytes, block, take);
		bytes += take;
		size -= take;
		crypto_wipe(block, sizeof(block));
		crypto_wipe(&context, sizeof(context));
	}

	/* Forward secrecy: the key that made these bytes is gone. */
	sha256_context_t context;

	sha256_init(&context);
	sha256_update(&context, "next", 4U);
	sha256_update(&context, g_random_key, sizeof(g_random_key));
	sha256_update(&context, &g_random_counter, sizeof(g_random_counter));
	sha256_final(&context, g_random_key);
	g_random_counter++;
	nxu_spin_unlock_irqrestore(&g_random_lock, state);
	crypto_wipe(&context, sizeof(context));
	crypto_wipe(&word, sizeof(word));
}

/* ---- the seed file -------------------------------------------------------- */

static bool
random_seed_read(uint8_t seed[RANDOM_SEED_SIZE])
{
	vnode_t vnode;
	uint64_t done = 0U;

	if (vfs_lookup(RANDOM_SEED_PATH, &vnode) != VFS_STATUS_OK) return false;
	while (done < RANDOM_SEED_SIZE) {
		uint64_t got = 0U;

		if (vnode_read(vnode, done, seed + done, RANDOM_SEED_SIZE - done, &got) != VFS_STATUS_OK || got == 0U) break;
		done += got;
	}
	vnode_rele(vnode);
	return done == RANDOM_SEED_SIZE;
}

static bool
random_seed_write(const uint8_t seed[RANDOM_SEED_SIZE])
{
	filedesc_t filedesc = &proc_kernel()->p_fd;
	uint32_t descriptor;
	uint64_t done = 0U;

	(void)vfs_mkdir("/Volumes/mainOS/var");
	(void)vfs_mkdir(RANDOM_SEED_DIRECTORY);
	if (vfs_open(filedesc, RANDOM_SEED_PATH, VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, &descriptor) !=
		VFS_STATUS_OK)
		return false;
	while (done < RANDOM_SEED_SIZE) {
		uint64_t written = 0U;

		if (vfs_write(filedesc, descriptor, seed + done, RANDOM_SEED_SIZE - done, &written) != VFS_STATUS_OK ||
			written == 0U)
			break;
		done += written;
	}
	(void)vfs_close(filedesc, descriptor);

	/* Only root reads it: anyone who could would know where this boot's numbers started. */
	vnode_t vnode;

	if (vfs_lookup(RANDOM_SEED_PATH, &vnode) == VFS_STATUS_OK) {
		vnode_attr_t attr = {0};

		attr.va_mode = 0600U;
		attr.va_uid = 0U;
		attr.va_gid = 0U;
		(void)vnode_setattr(vnode, &attr, VNODE_ATTR_MODE | VNODE_ATTR_UID | VNODE_ATTR_GID);
		vnode_rele(vnode);
	}
	return done == RANDOM_SEED_SIZE;
}

bool
random_seed_refresh(void)
{
	vnode_t vnode;
	uint8_t seed[RANDOM_SEED_SIZE];

	if (__atomic_load_n(&g_random_seeded_from_disk, __ATOMIC_ACQUIRE) != 0U) return true;
	if (vfs_lookup("/Volumes/mainOS/etc", &vnode) != VFS_STATUS_OK) return false;
	vnode_rele(vnode);
	if (__atomic_exchange_n(&g_random_seeded_from_disk, 1U, __ATOMIC_ACQ_REL) != 0U) return true;

	bool found = random_seed_read(seed);

	if (found) random_add_entropy(seed, sizeof(seed));
	random_bytes(seed, sizeof(seed));
	bool written = random_seed_write(seed);
	crypto_wipe(seed, sizeof(seed));

	kprintf("%s; %s\n", found ? "mixed in the stored seed" : "no stored seed yet",
			written ? "left a new one for the next boot" : "could not leave a new one");
	return true;
}
