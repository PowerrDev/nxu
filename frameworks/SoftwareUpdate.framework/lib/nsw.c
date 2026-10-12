/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <frameworks/SoftwareUpdate.framework/include/nsw.h>
#include <frameworks/CoreFoundation.framework/lib/plist/plist.h>
#include <libk/sha256.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#define NSW_SECTOR_SIZE 2048U
#define NSW_MANIFEST_MAX 8192U
#define NSW_PATH_MAX 256U

typedef struct {
	uint32_t block;
	uint32_t size;
	bool directory;
} nsw_extent_t;

typedef struct {
	int64_t descriptor;
	uint64_t size;
	uint64_t volume_size;
	nsw_extent_t root;
} nsw_iso_t;

static uint8_t g_sector[NSW_SECTOR_SIZE];
static uint8_t g_buffer[16384];
static char g_manifest[NSW_MANIFEST_MAX];

static void
nsw_log(const char *message)
{
	(void)nxu_write(1, message, nxu_strlen(message));
}

static uint32_t
nsw_le32(const uint8_t *bytes)
{
	return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static bool
nsw_dual32(const uint8_t *bytes, uint32_t *value)
{
	*value = nsw_le32(bytes);
	uint32_t big = (uint32_t)bytes[4] << 24 | (uint32_t)bytes[5] << 16 | (uint32_t)bytes[6] << 8 | bytes[7];
	return *value == big;
}

static bool
nsw_read(nsw_iso_t *iso, uint64_t offset, void *buffer, uint32_t size)
{
	if (offset > iso->size || size > iso->size - offset || nxu_seek(iso->descriptor, offset) < 0) {
		return false;
	}

	uint32_t complete = 0;
	while (complete < size) {
		int64_t count = nxu_read(iso->descriptor, (char *)buffer + complete, size - complete);
		if (count <= 0 || (uint64_t)count > size - complete) {
			return false;
		}
		complete += count;
	}
	return true;
}

static bool
nsw_extent(nsw_iso_t *iso, const uint8_t *record, uint32_t available, nsw_extent_t *extent)
{
	if (available < 34 || record[0] < 34 || record[0] > available || record[32] > record[0] - 33 || record[1] != 0 ||
		record[26] != 0 || record[27] != 0 || (record[25] & ~3U) != 0 || !nsw_dual32(record + 2, &extent->block) ||
		!nsw_dual32(record + 10, &extent->size)) {
		return false;
	}

	extent->directory = (record[25] & 2) != 0;
	uint64_t start = (uint64_t)extent->block * NSW_SECTOR_SIZE;
	return start <= iso->volume_size && extent->size <= iso->volume_size - start;
}

static bool
nsw_open(nsw_iso_t *iso, const char *path)
{
	nxu_stat_t info;
	if (nxu_stat(path, &info) < 0 || info.type != NXU_STAT_TYPE_REGULAR || info.size < 18 * NSW_SECTOR_SIZE ||
		info.size > UINT32_MAX) {
		return false;
	}
	iso->size = info.size;
	iso->descriptor = nxu_open(path, NXU_O_READ);
	if (iso->descriptor < 0) {
		return false;
	}

	nsw_log("patchd: reading NSW volume descriptors\n");
	bool found = false;
	bool terminated = false;
	for (uint32_t block = 16; block < 80; block++) {
		if (!nsw_read(iso, (uint64_t)block * NSW_SECTOR_SIZE, g_sector, sizeof(g_sector)) || g_sector[1] != 'C' ||
			g_sector[2] != 'D' || g_sector[3] != '0' || g_sector[4] != '0' || g_sector[5] != '1' || g_sector[6] != 1) {
			break;
		}
		if (g_sector[0] == 255) {
			terminated = true;
			break;
		}
		if (g_sector[0] != 2 || g_sector[88] != '%' || g_sector[89] != '/' ||
			(g_sector[90] != '@' && g_sector[90] != 'C' && g_sector[90] != 'E')) {
			continue;
		}
		uint32_t blocks;
		if (found || !nsw_dual32(g_sector + 80, &blocks) || blocks == 0 || g_sector[128] != 0 || g_sector[129] != 8 ||
			g_sector[130] != 8 || g_sector[131] != 0) {
			break;
		}
		iso->volume_size = (uint64_t)blocks * NSW_SECTOR_SIZE;
		if (iso->volume_size > iso->size || !nsw_extent(iso, g_sector + 156, 34, &iso->root) || !iso->root.directory) {
			break;
		}
		found = true;
	}

	if (!found || !terminated) {
		(void)nxu_close(iso->descriptor);
		return false;
	}
	return true;
}

static bool
nsw_name(const uint8_t *record, const char *name)
{
	uint32_t length = record[32];
	if ((length & 1) != 0 || length < 2) {
		return false;
	}
	if (length >= 4 && record[33 + length - 4] == 0 && record[33 + length - 3] == ';' && record[33 + length - 2] == 0 &&
		record[33 + length - 1] == '1') {
		length -= 4;
	}
	if (nxu_strlen(name) != length / 2) {
		return false;
	}
	for (uint32_t index = 0; index < length / 2; index++) {
		if (record[33 + index * 2] != 0 || record[34 + index * 2] != (uint8_t)name[index]) {
			return false;
		}
	}
	return true;
}

static bool
nsw_find(nsw_iso_t *iso, const nsw_extent_t *directory, const char *name, nsw_extent_t *result)
{
	if (!directory->directory || directory->size > 128 * NSW_SECTOR_SIZE) {
		return false;
	}
	bool found = false;
	for (uint32_t offset = 0; offset < directory->size; offset += NSW_SECTOR_SIZE) {
		uint32_t size = directory->size - offset;
		if (size > NSW_SECTOR_SIZE) {
			size = NSW_SECTOR_SIZE;
		}
		if (!nsw_read(iso, (uint64_t)directory->block * NSW_SECTOR_SIZE + offset, g_sector, size)) {
			return false;
		}
		for (uint32_t cursor = 0; cursor < size && g_sector[cursor] != 0;) {
			const uint8_t *record = g_sector + cursor;
			nsw_extent_t extent;
			if (!nsw_extent(iso, record, size - cursor, &extent)) {
				return false;
			}
			if (nsw_name(record, name)) {
				if (found) {
					return false;
				}
				*result = extent;
				found = true;
			}
			cursor += record[0];
		}
	}
	return found;
}

static bool
nsw_path(char output[NSW_PATH_MAX], const char *directory, const char *name)
{
	uint32_t prefix = nxu_strlen(directory);
	uint32_t suffix = nxu_strlen(name);
	if (prefix + suffix + 2 > NSW_PATH_MAX) {
		return false;
	}
	memcpy(output, directory, prefix);
	output[prefix++] = '/';
	memcpy(output + prefix, name, suffix + 1);
	return true;
}

static bool
nsw_write_file(const char *path, const void *bytes, uint32_t size)
{
	int64_t descriptor = nxu_open(path, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);
	if (descriptor < 0) {
		return false;
	}
	uint32_t complete = 0;
	while (complete < size) {
		int64_t count = nxu_write(descriptor, (const char *)bytes + complete, size - complete);
		if (count <= 0 || (uint64_t)count > size - complete) {
			(void)nxu_close(descriptor);
			return false;
		}
		complete += count;
	}
	return nxu_close(descriptor) == 0;
}

static void
nsw_progress(const char *path, uint32_t percent)
{
	char digits[4];
	uint32_t count = 0;
	if (percent >= 100) {
		digits[count++] = '1';
		digits[count++] = '0';
	} else if (percent >= 10) {
		digits[count++] = '0' + percent / 10;
	}
	digits[count++] = '0' + percent % 10;
	digits[count] = 0;
	nsw_log("patchd: extracting ");
	nsw_log(path);
	nsw_log(" ");
	nsw_log(digits);
	nsw_log("%\n");
}

static bool
nsw_extract(nsw_iso_t *iso, const nsw_extent_t *extent, const char *hash, const char *path)
{
	static const char *digits = "0123456789abcdef";
	if (extent->directory || extent->size < 2048 || nxu_strlen(hash) != 64 ||
		!nsw_read(iso, (uint64_t)extent->block * NSW_SECTOR_SIZE + 1080, g_buffer, 2) || g_buffer[0] != 0x53 ||
		g_buffer[1] != 0xef) {
		return false;
	}
	for (uint32_t index = 0; index < 64; index++) {
		bool valid = false;
		for (uint32_t digit = 0; digit < 16; digit++) {
			valid |= hash[index] == digits[digit];
		}
		if (!valid) {
			return false;
		}
	}

	int64_t descriptor = nxu_open(path, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);
	if (descriptor < 0) {
		return false;
	}
	sha256_context_t context;
	sha256_init(&context);
	bool success = true;
	uint32_t reported = 0;
	for (uint64_t offset = 0; offset < extent->size;) {
		uint32_t count = extent->size - offset;
		if (count > sizeof(g_buffer)) {
			count = sizeof(g_buffer);
		}
		if (!nsw_read(iso, (uint64_t)extent->block * NSW_SECTOR_SIZE + offset, g_buffer, count)) {
			success = false;
			break;
		}
		sha256_update(&context, g_buffer, count);
		uint32_t complete = 0;
		while (complete < count) {
			int64_t written = nxu_write(descriptor, g_buffer + complete, count - complete);
			if (written <= 0 || (uint64_t)written > count - complete) {
				success = false;
				break;
			}
			complete += written;
		}
		if (!success) {
			break;
		}
		offset += count;
		uint32_t percent = offset * 100 / extent->size;
		if (percent >= reported + 10 || offset == extent->size) {
			nsw_progress(path, percent);
			reported = percent;
		}
		(void)nxu_yield();
	}
	uint8_t digest[32];
	sha256_final(&context, digest);
	for (uint32_t index = 0; index < 32; index++) {
		success &= hash[index * 2] == digits[digest[index] >> 4] && hash[index * 2 + 1] == digits[digest[index] & 15];
	}
	if (nxu_close(descriptor) != 0) {
		success = false;
	}
	if (!success) {
		(void)nxu_unlink(path);
	}
	return success;
}

static bool
nsw_verify_file(const char *path, const char *hash, uint32_t size)
{
	nxu_stat_t info;
	if (nxu_stat(path, &info) < 0 || info.type != NXU_STAT_TYPE_REGULAR || info.size != size) {
		return false;
	}
	int64_t descriptor = nxu_open(path, NXU_O_READ);
	if (descriptor < 0) {
		return false;
	}

	sha256_context_t context;
	sha256_init(&context);
	int64_t count;
	while ((count = nxu_read(descriptor, g_buffer, sizeof(g_buffer))) > 0) {
		sha256_update(&context, g_buffer, count);
		(void)nxu_yield();
	}
	bool success = count == 0;
	uint8_t digest[32];
	sha256_final(&context, digest);
	const char *digits = "0123456789abcdef";
	for (uint32_t index = 0; index < 32; index++) {
		success &= hash[index * 2] == digits[digest[index] >> 4] && hash[index * 2 + 1] == digits[digest[index] & 15];
	}
	return nxu_close(descriptor) == 0 && success;
}

static bool
nsw_ready_available(const char *path)
{
	nxu_stat_t info;
	int64_t result = nxu_stat(path, &info);
	if (result == -NXU_SYS_E_NOT_FOUND) {
		return true;
	}
	if (result != 0 || info.type != NXU_STAT_TYPE_REGULAR) {
		return false;
	}

	int64_t descriptor = nxu_open(path, NXU_O_READ);
	if (descriptor < 0) {
		return false;
	}
	char contents[10] = {0};
	uint32_t complete = 0;
	int64_t count = 0;
	while (complete < 9 && (count = nxu_read(descriptor, contents + complete, 9 - complete)) > 0) {
		complete += count;
	}
	bool intact = info.size == 9 && complete == 9 && nxu_streq(contents, "verified\n");
	bool readable = nxu_close(descriptor) == 0 && count >= 0;
	/* A torn marker is not a committed build; remove it before retrying. */
	return readable && !intact && nxu_unlink(path) == 0 && nxu_sync() == 0;
}

bool
nsw_prepare(const char *package, const char *prepared_root)
{
	nsw_log("patchd: inspecting NSW package\n");
	nsw_iso_t iso = {0};
	nsw_extent_t directory, manifest_file, system, recovery;
	nsw_manifest_t manifest = {0};
	if (!nsw_open(&iso, package)) {
		nsw_log("patchd: invalid or unavailable NSW ISO/Joliet package\n");
		return false;
	}
	nsw_log("patchd: validating NSW manifest\n");
	bool success = nsw_find(&iso, &iso.root, ".nsw", &directory) &&
				   nsw_find(&iso, &directory, "BuildManifest.plist", &manifest_file) && !manifest_file.directory &&
				   manifest_file.size > 0 && manifest_file.size < sizeof(g_manifest) &&
				   nsw_read(&iso, (uint64_t)manifest_file.block * NSW_SECTOR_SIZE, g_manifest, manifest_file.size) &&
				   nsw_manifest_parse(g_manifest, manifest_file.size, &manifest) &&
				   nsw_find(&iso, &directory, manifest.system, &system) &&
				   nsw_find(&iso, &directory, manifest.recovery, &recovery);

	if (success) {
		nsw_log("patchd: preparing recovery destination\n");
	}

	char destination[NSW_PATH_MAX], path[NSW_PATH_MAX], ready[NSW_PATH_MAX], lock[NSW_PATH_MAX];
	bool locked = false;
	if (success) {
		success = nsw_path(destination, prepared_root, manifest.build) && nsw_path(ready, destination, "Ready");
	}
	if (success) {
		int64_t result = nxu_mkdir(prepared_root);
		success = result == 0 || result == -NXU_SYS_E_EXISTS;
		if (!success) {
			if (result == -NXU_SYS_E_DENIED) {
				nsw_log("patchd: filesystem-write capability missing\n");
				nsw_log("patchd: prepare from recovery\n");
			} else {
				nsw_log("patchd: unable to create the prepared-build directory\n");
			}
		}
	}
	if (success) {
		success = nsw_path(lock, prepared_root, ".prepare-lock") && nxu_mkdir(lock) == 0;
		locked = success;
		if (!success) {
			nsw_log("patchd: preparation is locked\npatchd: check for an active or interrupted update\n");
		}
	}
	if (success && !nsw_ready_available(ready)) {
		nsw_log("patchd: prepared build already exists or cannot be inspected\n");
		success = false;
	}
	if (success) {
		int64_t result = nxu_mkdir(destination);
		success = result == 0 || result == -NXU_SYS_E_EXISTS;
	}
	if (success) {
		nsw_log("patchd: extracting NSW system image\n");
		success =
			nsw_path(path, destination, manifest.system) && nsw_extract(&iso, &system, manifest.system_hash, path);
	}
	if (success) {
		nsw_log("patchd: extracting NSW recovery ramdisk\n");
		success = nsw_path(path, destination, manifest.recovery) &&
				  nsw_extract(&iso, &recovery, manifest.recovery_hash, path);
	}
	if (success) {
		success = nsw_path(path, destination, "BuildManifest.plist") &&
				  nsw_write_file(path, g_manifest, manifest_file.size) && nxu_sync() == 0;
	}
	if (success) {
		nsw_log("patchd: reading back staged images to verify their hashes\n");
		success = nsw_path(path, destination, manifest.system) &&
				  nsw_verify_file(path, manifest.system_hash, system.size) &&
				  nsw_path(path, destination, manifest.recovery) &&
				  nsw_verify_file(path, manifest.recovery_hash, recovery.size) &&
				  nsw_write_file(ready, "verified\n", 9) && nxu_sync() == 0;
		if (!success) {
			(void)nxu_unlink(ready);
			(void)nxu_sync();
		}
	}
	(void)nxu_close(iso.descriptor);
	if (locked) {
		(void)nxu_rmdir(lock);
		(void)nxu_sync();
	}
	if (success) {
		nsw_log("patchd: NSW system and recovery images verified and prepared\n");
	} else {
		nsw_log("patchd: NSW preparation failed\n");
		nsw_log("patchd: build is not ready\n");
	}
	return success;
}

static bool
nsw_text(const char *path, char *buffer, uint32_t capacity, uint32_t *length)
{
	int64_t descriptor = nxu_open(path, NXU_O_READ);
	if (descriptor < 0) {
		return false;
	}
	*length = 0;
	bool success = true;
	for (;;) {
		int64_t count = nxu_read(descriptor, buffer + *length, capacity - *length);
		if (count < 0 || (uint64_t)count >= capacity - *length) {
			success = false;
			break;
		}
		if (count == 0) {
			break;
		}
		*length += count;
	}
	buffer[*length] = 0;
	return nxu_close(descriptor) == 0 && success;
}

bool
nsw_schedule(const char *build)
{
	if (!nsw_component(build) || nxu_strlen(build) > 46) {
		return false;
	}
	char directory[256], path[256], lock[256];
	uint32_t length = 0;
	nsw_manifest_t manifest = {0};
	if (!nsw_path(directory, NSW_PREPARED_ROOT, build) || !nsw_path(lock, NSW_PREPARED_ROOT, ".prepare-lock") ||
		nxu_mkdir(lock) != 0) {
		nsw_log("patchd: update preparation is locked\n");
		return false;
	}
	char marker[16];
	nxu_stat_t info;
	bool success = nxu_stat(NSW_INSTALL_PENDING, &info) == -NXU_SYS_E_NOT_FOUND &&
				   nxu_stat(NSW_INSTALL_READY, &info) == -NXU_SYS_E_NOT_FOUND && nsw_path(path, directory, "Ready") &&
				   nsw_text(path, marker, sizeof(marker), &length) && nxu_streq(marker, "verified\n") &&
				   nsw_path(path, directory, "BuildManifest.plist") &&
				   nsw_text(path, g_manifest, sizeof(g_manifest), &length) &&
				   nsw_manifest_parse(g_manifest, length, &manifest) && nxu_streq(build, manifest.build);
	uint8_t digest[32];
	sha256(g_manifest, length, digest);
	if (success) {
		success = nsw_path(path, directory, manifest.system) && nxu_stat(path, &info) == 0 && info.size <= UINT32_MAX &&
				  nsw_verify_file(path, manifest.system_hash, info.size) &&
				  nsw_path(path, directory, manifest.recovery) && nxu_stat(path, &info) == 0 &&
				  info.size <= UINT32_MAX && nsw_verify_file(path, manifest.recovery_hash, info.size);
	}

	char transaction[96];
	if (success) {
		int64_t result = nxu_mkdir(NSW_TRANSACTIONS_ROOT);
		success = result == 0 || result == -NXU_SYS_E_EXISTS;
	}
	if (success) {
		uint32_t prefix = nxu_strlen(build);
		memcpy(transaction, build, prefix);
		transaction[prefix++] = '-';
		uint64_t nonce = nxu_uptime_us();
		const char *digits = "0123456789abcdef";
		success = false;
		for (uint32_t attempt = 0; attempt < 32; attempt++, nonce++) {
			for (uint32_t index = 0; index < 16; index++) {
				transaction[prefix + index] = digits[(nonce >> ((15 - index) * 4)) & 15];
			}
			transaction[prefix + 16] = 0;
			if (nsw_path(path, NSW_TRANSACTIONS_ROOT, transaction) && nxu_mkdir(path) == 0) {
				success = true;
				break;
			}
		}
	}
	if (success) {
		char pending[256];
		uint32_t offset = nxu_strlen(build);
		memcpy(pending, build, offset);
		pending[offset++] = '\n';
		uint32_t size = nxu_strlen(transaction);
		memcpy(pending + offset, transaction, size);
		offset += size;
		pending[offset++] = '\n';
		const char *digits = "0123456789abcdef";
		for (uint32_t index = 0; index < 32; index++) {
			pending[offset++] = digits[digest[index] >> 4];
			pending[offset++] = digits[digest[index] & 15];
		}
		pending[offset++] = '\n';
		success = nsw_write_file(NSW_INSTALL_PENDING, pending, offset) && nxu_sync() == 0 &&
				  nsw_write_file(NSW_INSTALL_READY, "queued\n", 7) && nxu_sync() == 0;
		if (!success) {
			(void)nxu_unlink(NSW_INSTALL_READY);
			(void)nxu_unlink(NSW_INSTALL_PENDING);
			(void)nxu_sync();
		}
	}
	(void)nxu_rmdir(lock);
	(void)nxu_sync();
	if (success) {
		nsw_log("patchd: installation queued for build ");
		nsw_log(build);
		nsw_log("\n");
		nsw_log("patchd: applying patches at the next boot\n");
	} else {
		nsw_log("patchd: installation request rejected\n");
	}
	return success;
}

bool
nsw_confirm_boot(void)
{
	char pending[256], directory[256], path[256], marker[32];
	uint32_t length;
	nsw_request_t request;
	if (!nsw_text(NSW_INSTALL_PENDING, pending, sizeof(pending), &length)) {
		return true;
	}
	if (!nsw_request_parse(pending, length, &request) ||
		!nsw_path(directory, NSW_TRANSACTIONS_ROOT, request.transaction) || !nsw_path(path, directory, "State") ||
		!nsw_text(path, marker, sizeof(marker), &length)) {
		return false;
	}
	if (!nxu_streq(marker, "trial\n")) {
		return false;
	}
	bool success = nsw_write_file(path, "committed\n", 10) && nxu_sync() == 0;
	if (success) {
		(void)nxu_unlink(NSW_INSTALL_READY);
		(void)nxu_unlink(NSW_INSTALL_PENDING);
		(void)nxu_unlink(NSW_ACTIVE_UPDATE);
		success = nxu_sync() == 0;
	}
	if (success) {
		nsw_log("patchd: updated system boot confirmed\n");
		nsw_log("patchd: rollback backups retained\n");
	}
	return success;
}
