/* Copyright (c) 2026 NXU Project. All rights reserved. */
/*
 * patchd's half of System Settings > General > Transfer or Reset ("Erase All
 * Content and Settings") and of Startup Disk. See doc/erase-and-startup-disk.md.
 *
 * Running as a service, patchd only takes requests: it checks them, writes a
 * durable marker or choice on the recovery volume and restarts. The erase
 * itself runs at the next boot, when the kernel starts the recovery volume's
 * copy of patchd as PID 1 with NSW_ERASE_ARGUMENT: nothing else is running,
 * and the program doing the work is not on the volume it wipes. It verifies
 * the whole factory payload first (nothing is touched if any of it is bad),
 * removes every file and directory on /Volumes/mainOS, reinstalls the
 * payload file by file, reads every file back to check its SHA-256, and
 * restarts into the fresh system. Every action is one log line, on the
 * console and in NSW_ERASE_LOG.
 */
#include "patchd_reset.h"

#include <frameworks/SoftwareUpdate.framework/include/nsw.h>
#include <libk/sha256.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

#define RESET_PATH_MAX 256U
#define RESET_LINE_MAX 768U
#define RESET_IO_SIZE 16384U
#define RESET_MANIFEST_MAX (512U * 1024U)
#define RESET_ENTRY_MAX 4096U
#define RESET_DEPTH_MAX 48U
#define RESET_ATTEMPTS 3U
#define RESET_MOUNTS_MAX 16U
#define RESET_SYSTEM "/Volumes/mainOS"
#define RESET_MANIFEST_HEADER "NXUFactoryManifest 1"

/* A log line is "function: text", on the console and in the erase log while it is open. */
#define RESET_LOG(...) reset_log(__func__, (const char *const[]){__VA_ARGS__, 0})

typedef struct {
	char kind;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint64_t size;
	uint8_t hash[32];
	const char *path;
} reset_entry_t;

static int64_t g_log = -1;
static char g_line[RESET_LINE_MAX];
static uint8_t g_io[RESET_IO_SIZE];
static char g_manifest[RESET_MANIFEST_MAX + 1U];
static reset_entry_t g_entries[RESET_ENTRY_MAX];
static uint32_t g_entry_count;
static char g_mounts[RESET_MOUNTS_MAX][RESET_PATH_MAX];
static uint32_t g_mount_count;
static uint64_t g_removed_files;
static uint64_t g_removed_directories;
static uint64_t g_removed_bytes;

/* ---- small helpers ------------------------------------------------------- */

/* Userland has no memcmp: nonzero when the two byte ranges differ. */
static int
reset_differ(const void *left, const void *right, uint32_t size)
{
	const uint8_t *a = left, *b = right;

	for (uint32_t index = 0; index < size; index++) {
		if (a[index] != b[index]) return 1;
	}
	return 0;
}

static void
reset_log(const char *function, const char *const *parts)
{
	uint32_t length = 0;
	const char *pieces[2] = {function, ": "};

	for (uint32_t index = 0; index < 2; index++) {
		for (const char *text = pieces[index]; *text != 0 && length < RESET_LINE_MAX - 2U; text++) {
			g_line[length++] = *text;
		}
	}

	for (uint32_t part = 0; parts[part] != 0; part++) {
		for (const char *text = parts[part]; *text != 0 && length < RESET_LINE_MAX - 2U; text++) {
			g_line[length++] = *text;
		}
	}

	g_line[length++] = '\n';
	(void)nxu_write(1U, g_line, length);
	if (g_log >= 0) (void)nxu_write((uint64_t)g_log, g_line, length);
}

static const char *
reset_number(char output[24], uint64_t value)
{
	char digits[24];
	uint32_t count = 0;

	do {
		digits[count++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0 && count < sizeof(digits));

	for (uint32_t index = 0; index < count; index++) {
		output[index] = digits[count - 1U - index];
	}
	output[count] = 0;
	return output;
}

static const char *
reset_octal(char output[8], uint32_t mode)
{
	for (uint32_t index = 0; index < 4; index++) {
		output[index] = (char)('0' + ((mode >> ((3U - index) * 3U)) & 7U));
	}
	output[4] = 0;
	return output;
}

static const char *
reset_hex(char output[65], const uint8_t digest[32])
{
	const char *digits = "0123456789abcdef";

	for (uint32_t index = 0; index < 32; index++) {
		output[index * 2U] = digits[digest[index] >> 4];
		output[index * 2U + 1U] = digits[digest[index] & 15U];
	}
	output[64] = 0;
	return output;
}

static bool
reset_join(char output[RESET_PATH_MAX], const char *base, const char *name)
{
	uint64_t prefix = nxu_strlen(base);
	uint64_t suffix = nxu_strlen(name);

	if (prefix + suffix + 2U > RESET_PATH_MAX) return false;

	memcpy(output, base, prefix);
	output[prefix] = '/';
	memcpy(output + prefix + 1U, name, suffix + 1U);
	return true;
}

static int64_t
reset_read_small(const char *path, char *bytes, uint64_t capacity)
{
	/* The whole file when it fits in capacity - 1 bytes (NUL-terminated), else a negative value. */
	int64_t descriptor = nxu_open(path, NXU_O_READ);

	if (descriptor < 0) return descriptor;

	uint64_t complete = 0;

	for (;;) {
		if (complete == capacity - 1U) {
			char extra;
			int64_t more = nxu_read((uint64_t)descriptor, &extra, 1U);
			(void)nxu_close((uint64_t)descriptor);
			return more == 0 ? (bytes[complete] = 0, (int64_t)complete) : -NXU_SYS_E_NO_SPACE;
		}

		int64_t length = nxu_read((uint64_t)descriptor, bytes + complete, capacity - 1U - complete);
		if (length < 0) {
			(void)nxu_close((uint64_t)descriptor);
			return length;
		}
		if (length == 0) break;
		complete += (uint64_t)length;
	}

	(void)nxu_close((uint64_t)descriptor);
	bytes[complete] = 0;
	return (int64_t)complete;
}

static bool
reset_write_text(const char *path, const char *text)
{
	int64_t descriptor = nxu_open(path, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);

	if (descriptor < 0) return false;

	uint64_t length = nxu_strlen(text);
	int64_t written = nxu_write((uint64_t)descriptor, text, length);
	bool closed = nxu_close((uint64_t)descriptor) == 0;
	return closed && written == (int64_t)length;
}

static bool
reset_text_is(const char *path, const char *expected)
{
	char bytes[96];
	int64_t length = reset_read_small(path, bytes, sizeof(bytes));

	return length >= 0 && nxu_streq(bytes, expected);
}

static bool
reset_exists(const char *path)
{
	nxu_stat_t info;
	return nxu_stat(path, &info) == 0;
}

static bool
reset_is_prefix(const char *text, const char *of)
{
	while (*text != 0) {
		if (*text++ != *of++) return false;
	}
	return true;
}

static bool
reset_starts_elf(const char *path)
{
	int64_t descriptor = nxu_open(path, NXU_O_READ);

	if (descriptor < 0) return false;

	uint8_t magic[4] = {0};
	int64_t length = nxu_read((uint64_t)descriptor, magic, sizeof(magic));
	(void)nxu_close((uint64_t)descriptor);
	return length == 4 && magic[0] == 0x7fU && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

static void
reset_restart(const char *why)
{
	RESET_LOG("restarting: ", why);
	if (g_log >= 0) {
		(void)nxu_close((uint64_t)g_log);
		g_log = -1;
	}
	(void)nxu_sync();
	(void)nxu_system_reset();
	RESET_LOG("reset is unavailable to this process");
}

/* ---- the factory manifest -------------------------------------------------- */

static bool
reset_parse_u32(const char **cursor, uint32_t base, uint32_t *value)
{
	const char *text = *cursor;
	uint64_t result = 0;
	uint32_t digits = 0;

	while (*text >= '0' && *text <= (base == 8U ? '7' : '9')) {
		result = result * base + (uint64_t)(*text - '0');
		if (result > 0xffffffffULL) return false;
		text++;
		digits++;
	}

	if (digits == 0 || *text != ' ') return false;

	*value = (uint32_t)result;
	*cursor = text + 1;
	return true;
}

static bool
reset_parse_u64(const char **cursor, uint64_t *value)
{
	const char *text = *cursor;
	uint64_t result = 0;
	uint32_t digits = 0;

	while (*text >= '0' && *text <= '9' && digits < 19U) {
		result = result * 10U + (uint64_t)(*text - '0');
		text++;
		digits++;
	}

	if (digits == 0 || *text != ' ') return false;

	*value = result;
	*cursor = text + 1;
	return true;
}

static bool
reset_relative_safe(const char *path)
{
	/* No leading slash, no empty, "." or ".." components, printable ASCII, short enough for the factory root. */
	uint64_t length = nxu_strlen(path);

	if (length == 0 || length + nxu_strlen(NSW_FACTORY_FILES) + 2U > RESET_PATH_MAX || path[0] == '/') return false;

	uint64_t start = 0;

	for (uint64_t index = 0; index <= length; index++) {
		char character = path[index];

		if (character != '/' && character != 0) {
			if ((uint8_t)character < 32U || (uint8_t)character > 126U) return false;
			continue;
		}

		uint64_t size = index - start;

		if (size == 0 || (size == 1 && path[start] == '.') ||
			(size == 2 && path[start] == '.' && path[start + 1U] == '.')) {
			return false;
		}

		start = index + 1U;
	}

	return true;
}

static bool
reset_manifest_load(const char **reason)
{
	/* Read the manifest, check it against its recorded SHA-256, then parse every line. */
	char expected_text[80];
	int64_t hash_length = reset_read_small(NSW_FACTORY_MANIFEST_HASH, expected_text, sizeof(expected_text));
	uint8_t expected[32], actual[32];

	if (hash_length != 65 || expected_text[64] != '\n') {
		*reason = "the factory manifest hash is missing or malformed";
		return false;
	}

	expected_text[64] = 0;
	if (!nsw_hash_decode(expected_text, expected)) {
		*reason = "the factory manifest hash is malformed";
		return false;
	}

	int64_t length = reset_read_small(NSW_FACTORY_MANIFEST, g_manifest, sizeof(g_manifest));
	if (length <= 0) {
		*reason = "the factory manifest is missing or too large";
		return false;
	}

	sha256(g_manifest, (size_t)length, actual);
	if (reset_differ(actual, expected, 32) != 0) {
		*reason = "the factory manifest does not match its SHA-256 (corrupt)";
		return false;
	}

	g_entry_count = 0;
	char *line = g_manifest;
	char *end = g_manifest + length;
	bool header = false, footer = false;

	while (line < end) {
		char *newline = line;
		while (newline < end && *newline != '\n') {
			newline++;
		}
		if (newline == end) {
			*reason = "the factory manifest does not end with a newline";
			return false;
		}
		*newline = 0;

		if (!header) {
			if (!nxu_streq(line, RESET_MANIFEST_HEADER)) {
				*reason = "the factory manifest has an unknown format";
				return false;
			}
			header = true;
		} else if (footer) {
			*reason = "the factory manifest continues after its end line";
			return false;
		} else if (line[0] == 'e' && line[1] == 'n' && line[2] == 'd' && line[3] == ' ') {
			const char *cursor = line + 4;
			uint64_t count = 0;
			while (*cursor >= '0' && *cursor <= '9') {
				count = count * 10U + (uint64_t)(*cursor++ - '0');
			}
			if (*cursor != 0 || count != g_entry_count) {
				*reason = "the factory manifest entry count does not match";
				return false;
			}
			footer = true;
		} else {
			if (g_entry_count == RESET_ENTRY_MAX || (line[0] != 'd' && line[0] != 'f') || line[1] != ' ') {
				*reason = "the factory manifest has a malformed entry";
				return false;
			}

			reset_entry_t *entry = &g_entries[g_entry_count];
			const char *cursor = line + 2;

			memset(entry, 0, sizeof(*entry));
			entry->kind = line[0];

			if (!reset_parse_u32(&cursor, 8U, &entry->mode) || entry->mode > 07777U ||
				!reset_parse_u32(&cursor, 10U, &entry->uid) || !reset_parse_u32(&cursor, 10U, &entry->gid) ||
				!reset_parse_u64(&cursor, &entry->size)) {
				*reason = "the factory manifest has a malformed entry";
				return false;
			}

			if (entry->kind == 'f') {
				char digest[65];
				memcpy(digest, cursor, 64);
				digest[64] = 0;
				if (nxu_strlen(cursor) < 66U || cursor[64] != ' ' || !nsw_hash_decode(digest, entry->hash)) {
					*reason = "the factory manifest has a malformed file hash";
					return false;
				}
				cursor += 65;
			} else {
				if (cursor[0] != '-' || cursor[1] != ' ') {
					*reason = "the factory manifest has a malformed directory entry";
					return false;
				}
				cursor += 2;
			}

			if (!reset_relative_safe(cursor)) {
				*reason = "the factory manifest names an unsafe path";
				return false;
			}

			entry->path = cursor;
			g_entry_count++;
		}

		line = newline + 1;
	}

	if (!header || !footer || g_entry_count == 0) {
		*reason = "the factory manifest is incomplete";
		return false;
	}

	return true;
}

/* ---- hashing and copying --------------------------------------------------- */

static bool
reset_hash_file(const char *path, uint8_t digest[32], uint64_t *size)
{
	int64_t descriptor = nxu_open(path, NXU_O_READ);

	if (descriptor < 0) return false;

	sha256_context_t context;
	bool success = true;

	sha256_init(&context);
	*size = 0;

	for (;;) {
		int64_t length = nxu_read((uint64_t)descriptor, g_io, sizeof(g_io));
		if (length < 0) {
			success = false;
			break;
		}
		if (length == 0) break;
		sha256_update(&context, g_io, (size_t)length);
		*size += (uint64_t)length;
	}

	(void)nxu_close((uint64_t)descriptor);
	sha256_final(&context, digest);
	return success;
}

static bool
reset_copy(const char *source, const char *target, uint8_t digest[32], uint64_t *size)
{
	/* Copy source to target, hashing the bytes read so a source that changed under us is caught too. */
	int64_t input = nxu_open(source, NXU_O_READ);

	if (input < 0) return false;

	int64_t output = nxu_open(target, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);
	if (output < 0) {
		(void)nxu_close((uint64_t)input);
		return false;
	}

	sha256_context_t context;
	bool success = true;

	sha256_init(&context);
	*size = 0;

	for (;;) {
		int64_t length = nxu_read((uint64_t)input, g_io, sizeof(g_io));
		if (length < 0) {
			success = false;
			break;
		}
		if (length == 0) break;

		sha256_update(&context, g_io, (size_t)length);
		*size += (uint64_t)length;

		uint64_t complete = 0;
		while (complete < (uint64_t)length) {
			int64_t written = nxu_write((uint64_t)output, g_io + complete, (uint64_t)length - complete);
			if (written <= 0) {
				success = false;
				break;
			}
			complete += (uint64_t)written;
		}

		if (!success) break;
	}

	sha256_final(&context, digest);
	if (nxu_close((uint64_t)output) != 0) success = false;
	(void)nxu_close((uint64_t)input);
	return success;
}

/* ---- service mode: Erase All Content and Settings --------------------------- */

static void
erase_status(const char *status)
{
	if (!reset_write_text(PATCHD_ERASE_STATUS, status)) {
		RESET_LOG("could not write ", PATCHD_ERASE_STATUS);
	}
	(void)nxu_sync();
}

static void
erase_refuse(const char *reason)
{
	char status[160] = "failed: ";
	uint64_t prefix = nxu_strlen(status);
	uint64_t length = nxu_strlen(reason);

	if (prefix + length + 2U > sizeof(status)) length = sizeof(status) - prefix - 2U;
	memcpy(status + prefix, reason, length);
	status[prefix + length] = '\n';
	status[prefix + length + 1U] = 0;

	RESET_LOG("erase not scheduled: ", reason);
	erase_status(status);
}

void
patchd_erase_requested(void)
{
	char request[96];
	int64_t length = reset_read_small(PATCHD_ERASE_REQUEST, request, sizeof(request));

	if (length == -NXU_SYS_E_NOT_FOUND) return;

	/* A prefix of the request without its newline is still being written: leave it for the next poll. */
	if (length >= 0 && reset_is_prefix(request, NSW_ERASE_MARKER) && !nxu_streq(request, NSW_ERASE_MARKER)) return;

	RESET_LOG("erase request found at ", PATCHD_ERASE_REQUEST);

	/* A stale "scheduled" or "failed" from an earlier attempt must never be shown for this one. */
	(void)nxu_unlink(PATCHD_ERASE_STATUS);

	if (nxu_unlink(PATCHD_ERASE_REQUEST) != 0 || nxu_sync() != 0) {
		RESET_LOG("unable to consume the erase request");
		erase_refuse("the request could not be consumed");
		return;
	}
	RESET_LOG("consumed the erase request");

	if (length < 0 || !nxu_streq(request, NSW_ERASE_MARKER)) {
		erase_refuse("invalid request (expected erase-all-content-and-settings)");
		return;
	}

	if (reset_exists(NSW_INSTALL_PENDING) || reset_exists(NSW_INSTALL_READY) || reset_exists(NSW_ACTIVE_UPDATE)) {
		erase_refuse("a software update is queued or in progress");
		return;
	}

	if (!reset_exists(NSW_RECOVERY_ROOT "/System/Recovery/triageOS")) {
		erase_refuse("the recovery volume is not mounted");
		return;
	}
	RESET_LOG("recovery volume present at ", NSW_RECOVERY_ROOT);

	if (!reset_starts_elf(NSW_ERASE_PROGRAM)) {
		erase_refuse("the recovery volume has no patchd to run the erase");
		return;
	}
	RESET_LOG("eraser present at ", NSW_ERASE_PROGRAM);

	const char *reason = 0;
	if (!reset_manifest_load(&reason)) {
		erase_refuse(reason);
		return;
	}

	char count[24];
	RESET_LOG("factory manifest verified: ", reset_number(count, g_entry_count), " entries in ", NSW_FACTORY_MANIFEST);

	int64_t made = nxu_mkdir(NSW_ERASE_ROOT);
	if (made != 0 && made != -NXU_SYS_E_EXISTS) {
		erase_refuse("could not create " NSW_ERASE_ROOT);
		return;
	}

	/* A new session: start the log afresh, then record the request in it. */
	g_log = nxu_open(NSW_ERASE_LOG, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);
	RESET_LOG("erase all content and settings requested from System Settings");
	RESET_LOG("factory manifest verified: ", count, " entries");

	(void)nxu_unlink(NSW_ERASE_FAILED);
	if (!reset_write_text(NSW_ERASE_QUEUED, NSW_ERASE_MARKER) || nxu_sync() != 0 ||
		!reset_text_is(NSW_ERASE_QUEUED, NSW_ERASE_MARKER)) {
		(void)nxu_unlink(NSW_ERASE_QUEUED);
		(void)nxu_sync();
		if (g_log >= 0) (void)nxu_close((uint64_t)g_log);
		g_log = -1;
		erase_refuse("the erase marker could not be written to the recovery volume");
		return;
	}

	RESET_LOG("wrote and synced ", NSW_ERASE_QUEUED);
	erase_status("scheduled\n");
	RESET_LOG("wrote scheduled to ", PATCHD_ERASE_STATUS);
	RESET_LOG("the erase runs from the recovery volume before userland at the next boot");
	reset_restart("to erase all content and settings");
	if (g_log >= 0) (void)nxu_close((uint64_t)g_log);
	g_log = -1;
	RESET_LOG("the erase remains queued for the next restart");
}

/* ---- service mode: Startup Disk -------------------------------------------- */

static const char *
startup_disk_stored(void)
{
	char choice[32];
	int64_t length = reset_read_small(NSW_STARTUP_DISK, choice, sizeof(choice));

	return length >= 0 && nxu_streq(choice, "Recovery\n") ? "Recovery\n" : "mainOS\n";
}

void
patchd_startup_disk_sync(void)
{
	const char *choice = startup_disk_stored();

	if (reset_text_is(NSW_STARTUP_DISK_MIRROR, choice)) return;

	if (reset_write_text(NSW_STARTUP_DISK_MIRROR, choice) && nxu_sync() == 0) {
		RESET_LOG("startup disk mirror updated: ", choice[0] == 'R' ? "Recovery" : "mainOS");
	} else {
		RESET_LOG("could not update ", NSW_STARTUP_DISK_MIRROR);
	}
}

void
patchd_startup_disk_requested(void)
{
	char request[64];
	int64_t length = reset_read_small(PATCHD_STARTUP_DISK_REQUEST, request, sizeof(request));

	if (length == -NXU_SYS_E_NOT_FOUND) return;

	/* No newline yet: still being written. */
	if (length >= 0 && length < (int64_t)sizeof(request) - 1 && (length == 0 || request[length - 1] != '\n')) return;

	RESET_LOG("startup disk request found at ", PATCHD_STARTUP_DISK_REQUEST);
	if (nxu_unlink(PATCHD_STARTUP_DISK_REQUEST) != 0 || nxu_sync() != 0) {
		RESET_LOG("unable to consume the startup disk request");
		return;
	}

	const char *volume = 0;
	bool now = false;

	if (length > 0) {
		request[length - 1] = 0;
		char *space = request;
		while (*space != 0 && *space != ' ') {
			space++;
		}
		if (*space == ' ') {
			*space = 0;
			const char *when = space + 1;
			if (nxu_streq(request, "mainOS")) volume = "mainOS\n";
			if (nxu_streq(request, "Recovery")) volume = "Recovery\n";
			if (nxu_streq(when, "now")) now = true;
			else if (!nxu_streq(when, "later")) volume = 0;
		}
	}

	if (volume == 0) {
		RESET_LOG("invalid startup disk request (expected \"mainOS|Recovery now|later\")");
		patchd_startup_disk_sync();
		return;
	}

	const char *name = volume[0] == 'R' ? "Recovery" : "mainOS";
	bool recovery_present = reset_exists(NSW_RECOVERY_ROOT "/System/Recovery/triageOS");

	if (!recovery_present && volume[0] == 'R') {
		RESET_LOG("startup disk Recovery refused: the recovery volume is not mounted");
		patchd_startup_disk_sync();
		return;
	}

	if (recovery_present) {
		if (!reset_write_text(NSW_STARTUP_DISK, volume) || nxu_sync() != 0 || !reset_text_is(NSW_STARTUP_DISK, volume)) {
			RESET_LOG("could not store the startup disk choice in ", NSW_STARTUP_DISK);
			patchd_startup_disk_sync();
			return;
		}
		RESET_LOG("startup disk set to ", name, " in ", NSW_STARTUP_DISK);
	}

	if (!reset_write_text(NSW_STARTUP_DISK_MIRROR, volume) || nxu_sync() != 0) {
		RESET_LOG("could not update ", NSW_STARTUP_DISK_MIRROR);
	} else {
		RESET_LOG("startup disk mirror updated: ", name);
	}

	if (volume[0] == 'R') RESET_LOG("Recovery is used for one boot, then the startup disk returns to mainOS");

	if (now) {
		reset_restart(volume[0] == 'R' ? "to start up from Recovery" : "to start up from mainOS");
	} else {
		RESET_LOG("the choice takes effect at the next restart");
	}
}

/* ---- PID 1 erase mode -------------------------------------------------------- */

static void
erase_collect_mounts(void)
{
	/* Anything mounted below the system volume is another filesystem: it is never wiped. */
	g_mount_count = 0;

	for (uint32_t index = 0; index < 64U && g_mount_count < RESET_MOUNTS_MAX; index++) {
		nxu_recovery_fs_mount_info_t info;
		if (nxu_recovery_fs_mount_info(index, &info) != 0) break;

		info.path[sizeof(info.path) - 1U] = 0;
		uint64_t length = nxu_strlen(RESET_SYSTEM);
		if (reset_is_prefix(RESET_SYSTEM "/", info.path) && nxu_strlen(info.path) > length + 1U) {
			memcpy(g_mounts[g_mount_count++], info.path, nxu_strlen(info.path) + 1U);
			RESET_LOG("keeping mounted filesystem ", info.path);
		}
	}
}

static bool
erase_skipped(const char *path, uint32_t depth, const char *name)
{
	if (depth == 0 && nxu_streq(name, "lost+found")) return true;

	for (uint32_t index = 0; index < g_mount_count; index++) {
		if (nxu_streq(g_mounts[index], path)) return true;
	}

	return false;
}

static bool
erase_unlink(const char *path, uint64_t size)
{
	char number[24];

	for (uint32_t attempt = 1; attempt <= RESET_ATTEMPTS; attempt++) {
		int64_t status = nxu_unlink(path);
		if (status == 0 || status == -NXU_SYS_E_NOT_FOUND) {
			RESET_LOG("removed file ", path, " (", reset_number(number, size), " bytes)");
			g_removed_files++;
			g_removed_bytes += size;
			return true;
		}
		char error[24];
		RESET_LOG("could not remove ", path, " (attempt ", reset_number(number, attempt), "), error ",
				  reset_number(error, (uint64_t)-status));
	}

	return false;
}

static bool
erase_remove_tree(char path[RESET_PATH_MAX], uint32_t depth)
{
	/*
	 * Take the first entry of the directory each time round, so removing it
	 * never disturbs a directory scan in progress, until only skipped
	 * entries are left.
	 */
	if (depth > RESET_DEPTH_MAX) {
		RESET_LOG("directory nesting too deep at ", path);
		return false;
	}

	uint64_t length = nxu_strlen(path);
	uint32_t skipped = 0;

	for (;;) {
		int64_t directory = nxu_open(path, NXU_O_READ);
		if (directory < 0) {
			RESET_LOG("cannot open directory ", path);
			return false;
		}

		nxu_dirent_t entry;
		int64_t result;
		uint32_t seen_skipped = 0;
		bool found = false;

		while ((result = nxu_readdir((uint64_t)directory, &entry)) > 0) {
			entry.name[NXU_DIRENT_NAME_MAX] = 0;
			if (nxu_streq(entry.name, ".") || nxu_streq(entry.name, "..")) continue;

			char child[RESET_PATH_MAX];
			if (!reset_join(child, path, entry.name)) {
				(void)nxu_close((uint64_t)directory);
				RESET_LOG("path too long below ", path);
				return false;
			}

			if (erase_skipped(child, depth, entry.name)) {
				seen_skipped++;
				continue;
			}

			found = true;
			break;
		}

		(void)nxu_close((uint64_t)directory);
		if (result < 0) {
			RESET_LOG("cannot read directory ", path);
			return false;
		}

		if (!found) {
			skipped = seen_skipped;
			break;
		}

		uint64_t name_length = nxu_strlen(entry.name);
		if (length + name_length + 2U > RESET_PATH_MAX) return false;
		path[length] = '/';
		memcpy(path + length + 1U, entry.name, name_length + 1U);

		nxu_stat_t info;
		bool removed;

		if (nxu_stat(path, &info) == 0 && info.type == NXU_STAT_TYPE_DIRECTORY) {
			removed = erase_remove_tree(path, depth + 1U);
		} else {
			removed = erase_unlink(path, nxu_stat(path, &info) == 0 ? info.size : 0);
		}

		path[length] = 0;
		if (!removed) return false;
	}

	if (depth == 0) return true;

	if (skipped != 0) {
		RESET_LOG("keeping directory ", path, "/ (it holds a mounted filesystem)");
		return true;
	}

	for (uint32_t attempt = 1; attempt <= RESET_ATTEMPTS; attempt++) {
		int64_t status = nxu_rmdir(path);
		if (status == 0 || status == -NXU_SYS_E_NOT_FOUND) {
			RESET_LOG("removed directory ", path, "/");
			g_removed_directories++;
			return true;
		}
		char number[24];
		RESET_LOG("could not remove directory ", path, "/ (attempt ", reset_number(number, attempt), ")");
	}

	return false;
}

static bool
erase_verify_payload(const char **reason)
{
	/* Every factory file must be present and match its size and SHA-256 before anything is removed. */
	char source[RESET_PATH_MAX], hex[65], number[24];
	uint64_t total = 0;

	for (uint32_t index = 0; index < g_entry_count; index++) {
		reset_entry_t *entry = &g_entries[index];

		if (!reset_join(source, NSW_FACTORY_FILES, entry->path)) {
			*reason = "a factory path is too long";
			return false;
		}

		nxu_stat_t info;
		if (nxu_stat(source, &info) != 0) {
			RESET_LOG("factory entry missing: ", source);
			*reason = "the factory payload is incomplete";
			return false;
		}

		if (entry->kind == 'd') {
			if (info.type != NXU_STAT_TYPE_DIRECTORY) {
				RESET_LOG("factory entry is not a directory: ", source);
				*reason = "the factory payload is corrupt";
				return false;
			}
			continue;
		}

		uint8_t digest[32];
		uint64_t size = 0;
		bool read = info.type == NXU_STAT_TYPE_REGULAR && reset_hash_file(source, digest, &size);

		if (!read || size != entry->size || reset_differ(digest, entry->hash, 32) != 0) {
			RESET_LOG("factory file corrupt: ", entry->path, " (", reset_number(number, size), " bytes, sha256 ",
					  read ? reset_hex(hex, digest) : "unreadable", ")");
			*reason = "the factory payload is corrupt";
			return false;
		}

		RESET_LOG("verified factory file ", entry->path, " (", reset_number(number, size), " bytes, sha256 ",
				  reset_hex(hex, digest), ")");
		total += size;
		(void)nxu_yield();
	}

	RESET_LOG("factory payload verified: ", reset_number(number, g_entry_count), " entries, ",
			  reset_number(hex, total), " bytes");
	return true;
}

static bool
erase_attributes(const char *target, const reset_entry_t *entry)
{
	/* Owner before mode, then read both back. */
	char mode[8], uid[24], gid[24];
	nxu_stat_t info;

	if (nxu_chown(target, entry->uid, entry->gid) != 0 || nxu_chmod(target, entry->mode) != 0 ||
		nxu_stat(target, &info) != 0 || (info.mode & 07777U) != entry->mode || info.uid != entry->uid ||
		info.gid != entry->gid) {
		RESET_LOG("could not set owner ", reset_number(uid, entry->uid), ":", reset_number(gid, entry->gid),
				  " and mode ", reset_octal(mode, entry->mode), " on ", target);
		return false;
	}

	return true;
}

static bool
erase_install_file(const reset_entry_t *entry, const char *source, const char *target)
{
	char hex[65], number[24], attempt_text[24], mode[8], uid[24], gid[24];

	for (uint32_t attempt = 1; attempt <= RESET_ATTEMPTS; attempt++) {
		uint8_t copied[32], readback[32];
		uint64_t copied_size = 0, readback_size = 0;

		(void)nxu_unlink(target);
		bool copy = reset_copy(source, target, copied, &copied_size);
		bool source_ok = copy && copied_size == entry->size && reset_differ(copied, entry->hash, 32) == 0;
		bool read = copy && reset_hash_file(target, readback, &readback_size);
		bool ok = source_ok && read && readback_size == entry->size && reset_differ(readback, entry->hash, 32) == 0;

		if (ok) {
			if (!erase_attributes(target, entry)) return false;
			RESET_LOG("installed ", target, " (", reset_number(number, readback_size), " bytes, sha256 ",
					  reset_hex(hex, readback), ", mode ", reset_octal(mode, entry->mode), ", owner ",
					  reset_number(uid, entry->uid), ":", reset_number(gid, entry->gid), "): verified");
			return true;
		}

		reset_number(attempt_text, attempt);
		if (!copy) {
			RESET_LOG("write failed for ", target, " (attempt ", attempt_text, ")");
		} else if (!source_ok) {
			RESET_LOG("factory source changed while copying ", source, " (attempt ", attempt_text, ")");
		} else {
			RESET_LOG("verification mismatch for ", target, ": read back ", reset_number(number, readback_size),
					  " bytes, sha256 ", read ? reset_hex(hex, readback) : "unreadable", " (attempt ", attempt_text,
					  ")");
		}
	}

	return false;
}

static bool
erase_install(const char **reason)
{
	char source[RESET_PATH_MAX], target[RESET_PATH_MAX], mode[8], number[24];
	uint64_t files = 0, directories = 0, bytes = 0;

	for (uint32_t index = 0; index < g_entry_count; index++) {
		const reset_entry_t *entry = &g_entries[index];

		if (!reset_join(source, NSW_FACTORY_FILES, entry->path) || !reset_join(target, RESET_SYSTEM, entry->path)) {
			*reason = "a path is too long";
			return false;
		}

		if (entry->kind == 'd') {
			int64_t status = nxu_mkdir(target);
			nxu_stat_t info;
			if ((status != 0 && status != -NXU_SYS_E_EXISTS) || nxu_stat(target, &info) != 0 ||
				info.type != NXU_STAT_TYPE_DIRECTORY || !erase_attributes(target, entry)) {
				RESET_LOG("could not create directory ", target, "/");
				*reason = "a directory could not be created";
				return false;
			}
			RESET_LOG("created directory ", target, "/ (mode ", reset_octal(mode, entry->mode), ")");
			directories++;
			continue;
		}

		if (!erase_install_file(entry, source, target)) {
			RESET_LOG("giving up on ", target, " after ", reset_number(number, RESET_ATTEMPTS), " attempts");
			*reason = "a reinstalled file did not verify (corruption)";
			return false;
		}

		files++;
		bytes += entry->size;
		(void)nxu_yield();
	}

	if (nxu_sync() != 0) {
		*reason = "the system volume could not be synced";
		return false;
	}

	char count[24], made[24];
	RESET_LOG("reinstalled ", reset_number(number, files), " files (", reset_number(count, bytes), " bytes) and ",
			  reset_number(made, directories), " directories");
	return true;
}

static bool
erase_final_check(const char **reason)
{
	/* After the sync, every installed file is read once more against the manifest. */
	char target[RESET_PATH_MAX], number[24];

	for (uint32_t index = 0; index < g_entry_count; index++) {
		const reset_entry_t *entry = &g_entries[index];

		if (entry->kind != 'f' || !reset_join(target, RESET_SYSTEM, entry->path)) continue;

		uint8_t digest[32];
		uint64_t size = 0;
		if (!reset_hash_file(target, digest, &size) || size != entry->size || reset_differ(digest, entry->hash, 32) != 0) {
			RESET_LOG("final verification failed for ", target);
			*reason = "the final verification found a corrupt file";
			return false;
		}
	}

	RESET_LOG("final verification passed for ", reset_number(number, g_entry_count), " entries");
	return true;
}

static void
erase_fail(const char *reason, bool system_changed)
{
	RESET_LOG("ERASE FAILED: ", reason);

	if (system_changed) {
		/* The system volume is incomplete: send every boot to recovery until it is retried there. */
		char text[160] = "failed: ";
		uint64_t prefix = nxu_strlen(text), length = nxu_strlen(reason);
		if (prefix + length + 2U > sizeof(text)) length = sizeof(text) - prefix - 2U;
		memcpy(text + prefix, reason, length);
		text[prefix + length] = '\n';
		text[prefix + length + 1U] = 0;

		bool marked = reset_write_text(NSW_ERASE_FAILED, text) && nxu_sync() == 0;
		RESET_LOG(marked ? "wrote " : "could not write ", NSW_ERASE_FAILED);
		RESET_LOG("the system volume is incomplete; the next boot starts recovery (run 'erase confirm' there to retry)");
	} else {
		/* Nothing was removed: report it to System Settings on the intact system and boot it normally. */
		char status[160] = "failed: ";
		uint64_t prefix = nxu_strlen(status), length = nxu_strlen(reason);
		if (prefix + length + 2U > sizeof(status)) length = sizeof(status) - prefix - 2U;
		memcpy(status + prefix, reason, length);
		status[prefix + length] = '\n';
		status[prefix + length + 1U] = 0;
		(void)reset_write_text(PATCHD_ERASE_STATUS, status);
		RESET_LOG("no file was changed; the next boot starts mainOS as it was");
	}

	(void)nxu_unlink(NSW_ERASE_QUEUED);
	(void)nxu_sync();
	RESET_LOG("removed ", NSW_ERASE_QUEUED);
	reset_restart("after the failed erase");

	for (;;) {
		(void)nxu_sleep_us(1000000ULL);
	}
}

int
patchd_erase_run(void)
{
	(void)nxu_mkdir(NSW_ERASE_ROOT);
	g_log = nxu_open(NSW_ERASE_LOG, NXU_O_WRITE | NXU_O_CREATE | NXU_O_APPEND);

	char number[24];
	RESET_LOG("erase all content and settings started as PID ", reset_number(number, (uint64_t)nxu_getpid()),
			  " from ", NSW_ERASE_PROGRAM);
	RESET_LOG(g_log >= 0 ? "logging every action to " : "could not open the log file ", NSW_ERASE_LOG);

	if (!reset_text_is(NSW_ERASE_QUEUED, NSW_ERASE_MARKER)) {
		RESET_LOG("no valid erase is queued in ", NSW_ERASE_QUEUED);
		erase_fail("the erase marker is missing or malformed", false);
	}

	const char *reason = 0;
	RESET_LOG("loading the factory manifest ", NSW_FACTORY_MANIFEST);
	if (!reset_manifest_load(&reason)) erase_fail(reason, false);
	RESET_LOG("factory manifest matches its SHA-256: ", reset_number(number, g_entry_count), " entries");

	RESET_LOG("verifying every byte of the factory payload in ", NSW_FACTORY_FILES);
	if (!erase_verify_payload(&reason)) erase_fail(reason, false);

	RESET_LOG("wiping ", RESET_SYSTEM, " (every file and directory, including Users and var)");
	erase_collect_mounts();
	static char path[RESET_PATH_MAX];
	memcpy(path, RESET_SYSTEM, sizeof(RESET_SYSTEM));
	g_removed_files = g_removed_directories = g_removed_bytes = 0;
	bool wiped = erase_remove_tree(path, 0);
	(void)nxu_sync();

	char directories[24], bytes[24];
	RESET_LOG("removed ", reset_number(number, g_removed_files), " files (", reset_number(bytes, g_removed_bytes),
			  " bytes) and ", reset_number(directories, g_removed_directories), " directories");
	if (!wiped) erase_fail("the system volume could not be wiped completely", true);
	if (g_log >= 0) (void)nxu_sync();
	RESET_LOG(RESET_SYSTEM, " is empty");

	RESET_LOG("reinstalling ", reset_number(number, g_entry_count), " entries from the factory payload");
	if (!erase_install(&reason)) erase_fail(reason, true);
	if (!erase_final_check(&reason)) erase_fail(reason, true);

	(void)nxu_unlink(NSW_ERASE_FAILED);
	if (nxu_unlink(NSW_ERASE_QUEUED) != 0 || nxu_sync() != 0) {
		RESET_LOG("could not remove ", NSW_ERASE_QUEUED, "; the next boot would erase again");
		erase_fail("the erase marker could not be cleared", true);
	}
	RESET_LOG("removed ", NSW_ERASE_QUEUED);

	if (reset_write_text(NSW_STARTUP_DISK, "mainOS\n")) RESET_LOG("startup disk set to mainOS");
	if (reset_write_text(NSW_STARTUP_DISK_MIRROR, "mainOS\n")) RESET_LOG("wrote ", NSW_STARTUP_DISK_MIRROR);

	RESET_LOG("erase all content and settings completed successfully");
	reset_restart("into the freshly installed system");

	for (;;) {
		(void)nxu_sleep_us(1000000ULL);
	}
}
