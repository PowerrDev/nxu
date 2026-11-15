/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/security/passcode.c
 *
 * See kern/security/passcode.h.
 *
 * The record is text, padded with newlines to PASSCODE_RECORD_SIZE bytes so
 * that every later update overwrites it in place, one block, without
 * truncating it first:
 *
 *   nxu-passcode 1
 *   hash pbkdf2_sha256$<iterations>$<salt-hex>$<hash-hex>
 *   failures <n>
 *   last <unix time of the last failure>
 *
 * Missing or empty means no passcode is set. Present but unreadable means
 * locked: a damaged record never turns into "no passcode".
 *
 * Logs say what happened, never anything about the passcode itself.
 */

#include <kern/security/passcode.h>

#include <kern/console/console.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <kern/security/random.h>
#include <libk/pbkdf2.h>
#include <libk/sha256.h>
#include <libk/string.h>
#include <platform/rtc.h>
#include <vfs/file.h>
#include <vfs/vfs.h>
#include <vfs/vnode.h>

#define PASSCODE_VOLUME_PROBE "/Volumes/mainOS/etc"
#define PASSCODE_DIRECTORY "/Volumes/mainOS/var/db"
#define PASSCODE_RECORD_SIZE 256U
#define PASSCODE_ITERATIONS 20000U
#define PASSCODE_SALT_SIZE 16U
#define PASSCODE_FIRST_DELAYED 5U
#define PASSCODE_LOCKOUT 10U

/* Seconds to wait after the 5th..9th failure in a row. */
static const uint32_t g_passcode_delays[PASSCODE_LOCKOUT - PASSCODE_FIRST_DELAYED] = {60U, 300U, 900U, 3600U, 3600U};

typedef struct {
	bool set;
	bool damaged;
	uint32_t iterations;
	uint8_t salt[PASSCODE_SALT_SIZE];
	uint8_t hash[SHA256_DIGEST_SIZE];
	uint32_t failures;
	uint64_t last_failure;
	uint64_t file_size; /* 0 when there is no file */
} passcode_record_t;

/* One caller at a time; the holder may sleep in the filesystem, so this is not a spinlock. */
static volatile uint32_t g_passcode_busy;

static void
passcode_lock(void)
{
	while (__atomic_exchange_n(&g_passcode_busy, 1U, __ATOMIC_ACQUIRE) != 0U)
		(void)sched_sleep_us(1000U);
}

static void
passcode_unlock(void)
{
	__atomic_store_n(&g_passcode_busy, 0U, __ATOMIC_RELEASE);
}

const char *
passcode_result_name(passcode_result_t result)
{
	switch (result) {
	case PASSCODE_OK:
		return "ok";
	case PASSCODE_ERR_UNAVAILABLE:
		return "unavailable";
	case PASSCODE_ERR_INVALID:
		return "invalid";
	case PASSCODE_ERR_NOT_FOUND:
		return "no passcode set";
	case PASSCODE_ERR_DENIED:
		return "denied";
	case PASSCODE_ERR_RETRY_LATER:
		return "retry later";
	case PASSCODE_ERR_LOCKED:
		return "locked";
	}
	return "unknown";
}

/* ---- parsing and formatting ----------------------------------------------- */

static int
passcode_hex_digit(char character)
{
	if (character >= '0' && character <= '9') return character - '0';
	if (character >= 'a' && character <= 'f') return character - 'a' + 10;
	if (character >= 'A' && character <= 'F') return character - 'A' + 10;
	return -1;
}

static bool
passcode_parse_hex(const char *text, size_t length, uint8_t *out, size_t size)
{
	if (length != size * 2U) return false;
	for (size_t index = 0U; index < size; index++) {
		int high = passcode_hex_digit(text[index * 2U]);
		int low = passcode_hex_digit(text[index * 2U + 1U]);

		if (high < 0 || low < 0) return false;
		out[index] = (uint8_t)((high << 4) | low);
	}
	return true;
}

static bool
passcode_parse_number(const char *text, size_t length, uint64_t *value)
{
	uint64_t number = 0U;

	if (length == 0U || length > 19U) return false;
	for (size_t index = 0U; index < length; index++) {
		if (text[index] < '0' || text[index] > '9') return false;
		number = number * 10U + (uint64_t)(text[index] - '0');
	}
	*value = number;
	return true;
}

static bool
passcode_starts(const char *line, size_t length, const char *word, size_t *rest)
{
	size_t word_length = strlen(word);

	if (length <= word_length || memcmp(line, word, word_length) != 0 || line[word_length] != ' ') return false;
	*rest = word_length + 1U;
	return true;
}

/* "pbkdf2_sha256$<iterations>$<salt>$<hash>" */
static bool
passcode_parse_hash(const char *text, size_t length, passcode_record_t *record)
{
	static const char prefix[] = "pbkdf2_sha256$";
	size_t at = sizeof(prefix) - 1U;
	size_t fields[3] = {0U, 0U, 0U}, ends[3] = {0U, 0U, 0U};
	uint64_t iterations;

	if (length <= at || memcmp(text, prefix, at) != 0) return false;
	for (uint32_t field = 0U; field < 3U; field++) {
		fields[field] = at;
		while (at < length && text[at] != '$')
			at++;
		ends[field] = at;
		if (field < 2U) {
			if (at >= length) return false;
			at++;
		}
	}
	if (ends[2] != length) return false;
	if (!passcode_parse_number(text + fields[0], ends[0] - fields[0], &iterations) || iterations == 0U ||
		iterations > 10000000U)
		return false;
	if (!passcode_parse_hex(text + fields[1], ends[1] - fields[1], record->salt, sizeof(record->salt))) return false;
	if (!passcode_parse_hex(text + fields[2], ends[2] - fields[2], record->hash, sizeof(record->hash))) return false;
	record->iterations = (uint32_t)iterations;
	return true;
}

static void
passcode_parse(const char *text, size_t size, passcode_record_t *record)
{
	bool header = false, hash = false;
	size_t start = 0U;

	while (start < size) {
		size_t end = start;

		while (end < size && text[end] != '\n')
			end++;

		const char *line = text + start;
		size_t length = end - start, rest;
		uint64_t number;

		if (length == 0U) {
			/* padding */
		} else if (!header) {
			header = length == 14U && memcmp(line, "nxu-passcode 1", 14U) == 0;
			if (!header) break;
		} else if (passcode_starts(line, length, "hash", &rest)) {
			hash = passcode_parse_hash(line + rest, length - rest, record);
			if (!hash) break;
		} else if (passcode_starts(line, length, "failures", &rest)) {
			if (!passcode_parse_number(line + rest, length - rest, &number)) break;
			record->failures = number > PASSCODE_LOCKOUT ? PASSCODE_LOCKOUT : (uint32_t)number;
		} else if (passcode_starts(line, length, "last", &rest)) {
			if (!passcode_parse_number(line + rest, length - rest, &number)) break;
			record->last_failure = number;
		} else {
			break;
		}
		start = end + 1U;
	}

	if (header && hash && start >= size) {
		record->set = true;
	} else {
		record->damaged = true;
	}
}

static void
passcode_append(char *out, size_t capacity, size_t *used, const char *text)
{
	while (*text != '\0' && *used + 1U < capacity)
		out[(*used)++] = *text++;
	out[*used] = '\0';
}

static void
passcode_append_number(char *out, size_t capacity, size_t *used, uint64_t value)
{
	char digits[24];
	size_t count = 0U;

	do {
		digits[count++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0U && count < sizeof(digits));
	while (count != 0U && *used + 1U < capacity)
		out[(*used)++] = digits[--count];
	out[*used] = '\0';
}

static void
passcode_append_hex(char *out, size_t capacity, size_t *used, const uint8_t *bytes, size_t size)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t index = 0U; index < size && *used + 2U < capacity; index++) {
		out[(*used)++] = digits[bytes[index] >> 4];
		out[(*used)++] = digits[bytes[index] & 15U];
	}
	out[*used] = '\0';
}

/* The whole record, padded with newlines to exactly PASSCODE_RECORD_SIZE bytes. */
static void
passcode_format(const passcode_record_t *record, char out[PASSCODE_RECORD_SIZE + 1U])
{
	size_t used = 0U;

	out[0] = '\0';
	passcode_append(out, PASSCODE_RECORD_SIZE + 1U, &used, "nxu-passcode 1\nhash pbkdf2_sha256$");
	passcode_append_number(out, PASSCODE_RECORD_SIZE + 1U, &used, record->iterations);
	passcode_append(out, PASSCODE_RECORD_SIZE + 1U, &used, "$");
	passcode_append_hex(out, PASSCODE_RECORD_SIZE + 1U, &used, record->salt, sizeof(record->salt));
	passcode_append(out, PASSCODE_RECORD_SIZE + 1U, &used, "$");
	passcode_append_hex(out, PASSCODE_RECORD_SIZE + 1U, &used, record->hash, sizeof(record->hash));
	passcode_append(out, PASSCODE_RECORD_SIZE + 1U, &used, "\nfailures ");
	passcode_append_number(out, PASSCODE_RECORD_SIZE + 1U, &used, record->failures);
	passcode_append(out, PASSCODE_RECORD_SIZE + 1U, &used, "\nlast ");
	passcode_append_number(out, PASSCODE_RECORD_SIZE + 1U, &used, record->last_failure);
	passcode_append(out, PASSCODE_RECORD_SIZE + 1U, &used, "\n");
	while (used < PASSCODE_RECORD_SIZE)
		out[used++] = '\n';
	out[PASSCODE_RECORD_SIZE] = '\0';
}

/* ---- the file ----------------------------------------------------------------- */

static bool
passcode_volume_mounted(void)
{
	vnode_t vnode;

	if (vfs_lookup(PASSCODE_VOLUME_PROBE, &vnode) != VFS_STATUS_OK) return false;
	vnode_rele(vnode);
	return true;
}

/* False when the system volume is not there yet. */
static bool
passcode_load(passcode_record_t *record)
{
	vnode_t vnode;
	char text[PASSCODE_RECORD_SIZE * 2U];
	uint64_t done = 0U;

	memset(record, 0, sizeof(*record));
	if (!passcode_volume_mounted()) return false;

	/* The boot's random numbers get the stored seed as soon as there is a volume to keep it on. */
	(void)random_seed_refresh();

	if (vfs_lookup(PASSCODE_RECORD_PATH, &vnode) != VFS_STATUS_OK) return true;

	record->file_size = vnode->v_size;
	if (vnode->v_type != VNODE_TYPE_REGULAR || vnode->v_size > sizeof(text)) {
		vnode_rele(vnode);
		record->damaged = true;
		return true;
	}
	while (done < vnode->v_size) {
		uint64_t got = 0U;

		if (vnode_read(vnode, done, text + done, vnode->v_size - done, &got) != VFS_STATUS_OK || got == 0U) break;
		done += got;
	}
	vnode_rele(vnode);

	if (done != record->file_size) {
		record->damaged = true;
	} else if (done != 0U) {
		passcode_parse(text, (size_t)done, record);
	}
	crypto_wipe(text, sizeof(text));
	return true;
}

static bool
passcode_store(passcode_record_t *record)
{
	filedesc_t filedesc = &proc_kernel()->p_fd;
	char text[PASSCODE_RECORD_SIZE + 1U];
	uint32_t descriptor;
	uint64_t done = 0U;
	/* Same size: overwrite in place. Anything else: start the file over. */
	uint32_t flags = record->file_size == PASSCODE_RECORD_SIZE ? VFS_OPEN_WRITE
															   : VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE;

	passcode_format(record, text);
	(void)vfs_mkdir("/Volumes/mainOS/var");
	(void)vfs_mkdir(PASSCODE_DIRECTORY);
	if (vfs_open(filedesc, PASSCODE_RECORD_PATH, flags, &descriptor) != VFS_STATUS_OK) {
		kprintf("could not open %s\n", PASSCODE_RECORD_USER_PATH);
		crypto_wipe(text, sizeof(text));
		return false;
	}
	while (done < PASSCODE_RECORD_SIZE) {
		uint64_t written = 0U;

		if (vfs_write(filedesc, descriptor, text + done, PASSCODE_RECORD_SIZE - done, &written) != VFS_STATUS_OK ||
			written == 0U)
			break;
		done += written;
	}
	(void)vfs_close(filedesc, descriptor);
	crypto_wipe(text, sizeof(text));

	if (record->file_size != PASSCODE_RECORD_SIZE) {
		vnode_t vnode;

		if (vfs_lookup(PASSCODE_RECORD_PATH, &vnode) == VFS_STATUS_OK) {
			vnode_attr_t attr = {0};

			attr.va_mode = 0600U;
			attr.va_uid = 0U;
			attr.va_gid = 0U;
			(void)vnode_setattr(vnode, &attr, VNODE_ATTR_MODE | VNODE_ATTR_UID | VNODE_ATTR_GID);
			vnode_rele(vnode);
		}
	}
	if (done != PASSCODE_RECORD_SIZE) {
		kprintf("could not write all of %s\n", PASSCODE_RECORD_USER_PATH);
		return false;
	}
	record->file_size = PASSCODE_RECORD_SIZE;

	/* On the disk before the answer: a failure counted is a failure kept. */
	(void)vfs_sync_all();
	return true;
}

/* ---- policy ------------------------------------------------------------------- */

static uint32_t
passcode_wait(const passcode_record_t *record)
{
	if (record->failures < PASSCODE_FIRST_DELAYED || record->failures >= PASSCODE_LOCKOUT) return 0U;

	uint64_t delay = g_passcode_delays[record->failures - PASSCODE_FIRST_DELAYED];
	uint64_t now = rtc_unix_time();
	/* A clock set back does not shorten the wait: it starts again. */
	uint64_t elapsed = now >= record->last_failure ? now - record->last_failure : 0U;

	return elapsed >= delay ? 0U : (uint32_t)(delay - elapsed);
}

static bool
passcode_locked(const passcode_record_t *record)
{
	return record->damaged || record->failures >= PASSCODE_LOCKOUT;
}

/*
 * Check a passcode against the record, counting the attempt on disk first.
 * Called with the lock held.
 */
static passcode_result_t
passcode_check_locked(passcode_record_t *record, const void *passcode, uint32_t length, uint32_t *wait_seconds)
{
	uint8_t computed[SHA256_DIGEST_SIZE];

	if (record->damaged) {
		kprintf("%s is damaged: the passcode stays locked until it is removed from Recovery\n",
				PASSCODE_RECORD_USER_PATH);
		return PASSCODE_ERR_LOCKED;
	}
	if (!record->set) return PASSCODE_ERR_NOT_FOUND;
	if (passcode_locked(record)) return PASSCODE_ERR_LOCKED;

	uint32_t wait = passcode_wait(record);

	if (wait != 0U) {
		*wait_seconds = wait;
		return PASSCODE_ERR_RETRY_LATER;
	}
	if (passcode == 0 || length < PASSCODE_MIN_LENGTH || length > PASSCODE_MAX_LENGTH) return PASSCODE_ERR_INVALID;

	uint32_t failures = record->failures;

	record->failures = failures + 1U;
	record->last_failure = rtc_unix_time();
	if (!passcode_store(record)) {
		/* Not counted, so not compared. */
		record->failures = failures;
		return PASSCODE_ERR_UNAVAILABLE;
	}

	pbkdf2_hmac_sha256(passcode, length, record->salt, sizeof(record->salt), record->iterations, computed,
					   sizeof(computed));
	bool match = crypto_equal(computed, record->hash, sizeof(computed));

	crypto_wipe(computed, sizeof(computed));

	if (!match) {
		kprintf("passcode refused (%u failure%s in a row)\n", (unsigned)record->failures,
				record->failures == 1U ? "" : "s");
		if (record->failures >= PASSCODE_LOCKOUT) kprintf("too many failures: the passcode is locked\n");
		return PASSCODE_ERR_DENIED;
	}

	record->failures = 0U;
	record->last_failure = 0U;
	if (!passcode_store(record)) kprintf("could not clear the failure count\n");
	return PASSCODE_OK;
}

/* ---- the interface ------------------------------------------------------------ */

passcode_result_t
passcode_status(passcode_status_t *status)
{
	passcode_record_t record;

	if (status == 0) return PASSCODE_ERR_INVALID;
	memset(status, 0, sizeof(*status));

	passcode_lock();
	bool mounted = passcode_load(&record);
	passcode_unlock();

	if (!mounted) return PASSCODE_ERR_UNAVAILABLE;

	status->passcode_set = record.set || record.damaged;
	status->locked = passcode_locked(&record);
	status->failures = (uint8_t)record.failures;
	status->wait_seconds = status->locked ? 0U : passcode_wait(&record);
	crypto_wipe(&record, sizeof(record));
	return PASSCODE_OK;
}

passcode_result_t
passcode_verify(const void *passcode, uint32_t length, uint32_t *wait_seconds)
{
	passcode_record_t record;
	uint32_t ignored = 0U;
	passcode_result_t result;

	if (wait_seconds == 0) wait_seconds = &ignored;
	*wait_seconds = 0U;

	passcode_lock();
	if (!passcode_load(&record))
		result = PASSCODE_ERR_UNAVAILABLE;
	else
		result = passcode_check_locked(&record, passcode, length, wait_seconds);
	passcode_unlock();

	crypto_wipe(&record, sizeof(record));
	if (result == PASSCODE_OK) kprintf("passcode accepted\n");
	return result;
}

passcode_result_t
passcode_set(const void *old_passcode, uint32_t old_length, const void *passcode, uint32_t length,
			 uint32_t *wait_seconds)
{
	passcode_record_t record;
	uint32_t ignored = 0U;
	passcode_result_t result = PASSCODE_OK;

	if (wait_seconds == 0) wait_seconds = &ignored;
	*wait_seconds = 0U;
	if (passcode == 0 || length < PASSCODE_MIN_LENGTH || length > PASSCODE_MAX_LENGTH) return PASSCODE_ERR_INVALID;

	passcode_lock();
	if (!passcode_load(&record)) {
		result = PASSCODE_ERR_UNAVAILABLE;
	} else if (record.set || record.damaged) {
		/* Changing it: the old one first, under the same delays as any attempt. */
		result = old_length == 0U ? PASSCODE_ERR_INVALID
								  : passcode_check_locked(&record, old_passcode, old_length, wait_seconds);
	} else if (old_length != 0U) {
		result = PASSCODE_ERR_INVALID;
	}

	if (result == PASSCODE_OK) {
		record.set = true;
		record.damaged = false;
		record.iterations = PASSCODE_ITERATIONS;
		record.failures = 0U;
		record.last_failure = 0U;
		random_bytes(record.salt, sizeof(record.salt));
		pbkdf2_hmac_sha256(passcode, length, record.salt, sizeof(record.salt), record.iterations, record.hash,
						   sizeof(record.hash));
		result = passcode_store(&record) ? PASSCODE_OK : PASSCODE_ERR_UNAVAILABLE;
	}
	passcode_unlock();

	crypto_wipe(&record, sizeof(record));
	kprintf("%s\n", result == PASSCODE_OK ? "passcode set" : "the passcode was not set");
	return result;
}
