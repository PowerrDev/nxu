/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <frameworks/SoftwareUpdate.framework/include/nsw.h>
#include <frameworks/CoreFoundation.framework/lib/plist/plist.h>

static uint32_t
manifest_length(const char *text)
{
	uint32_t length = 0;
	while (text[length] != 0) {
		length++;
	}
	return length;
}

static bool
manifest_equal(const char *left, const char *right)
{
	while (*left != 0 && *left == *right) {
		left++;
		right++;
	}
	return *left == *right;
}

bool
nsw_component(const char *name)
{
	uint32_t length = manifest_length(name);
	if (length == 0 || manifest_equal(name, ".") || manifest_equal(name, "..")) {
		return false;
	}
	for (uint32_t index = 0; index < length; index++) {
		char character = name[index];
		if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			  (character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.')) {
			return false;
		}
	}
	return true;
}

bool
nsw_manifest_parse(const char *data, uint32_t length, nsw_manifest_t *manifest)
{
	static const char *keys[] = {
		"FormatVersion",		"Build",		   "Architecture",			"SystemImage",
		"SystemImageSHA256",	"RecoveryRamdisk", "RecoveryRamdiskSHA256", "SystemImageFormat",
		"RecoveryRamdiskFormat"};
	char *values[] = {0,
					  manifest->build,
					  manifest->architecture,
					  manifest->system,
					  manifest->system_hash,
					  manifest->recovery,
					  manifest->recovery_hash,
					  0,
					  0};
	uint32_t capacities[] = {0, 64, 16, 128, 65, 128, 65, 0, 0};
	plist_parser_t parser;
	plist_event_t event;
	uint32_t seen = 0;
	if (plist_parser_init(&parser, data, length) != PLIST_STATUS_OK ||
		plist_parser_next(&parser, &event) != PLIST_STATUS_OK || event.type != PLIST_EVENT_DICT_BEGIN) {
		return false;
	}

	for (;;) {
		if (plist_parser_next(&parser, &event) != PLIST_STATUS_OK) {
			return false;
		}
		if (event.type == PLIST_EVENT_DICT_END) {
			break;
		}
		if (event.type != PLIST_EVENT_KEY) {
			return false;
		}
		uint32_t index;
		for (index = 0; index < 9 && !manifest_equal(event.text, keys[index]); index++) {
		}
		if (index == 9 || (seen & (1U << index)) != 0 || plist_parser_next(&parser, &event) != PLIST_STATUS_OK) {
			return false;
		}
		seen |= 1U << index;
		if (index == 0) {
			if (event.type != PLIST_EVENT_INTEGER || event.integer != 1) {
				return false;
			}
		} else if (event.type != PLIST_EVENT_STRING) {
			return false;
		} else if (index >= 7) {
			if (!manifest_equal(event.text, "ext4")) {
				return false;
			}
		} else {
			uint32_t size = manifest_length(event.text) + 1;
			if (size > capacities[index]) {
				return false;
			}
			for (uint32_t character = 0; character < size; character++) {
				values[index][character] = event.text[character];
			}
		}
	}

#if defined(__i386__)
	const char *architecture = "i386";
#else
	const char *architecture = "arm64";
#endif
	return seen == 511 && plist_parser_next(&parser, &event) == PLIST_STATUS_END &&
		   manifest_equal(manifest->architecture, architecture) && nsw_component(manifest->build) &&
		   !manifest_equal(manifest->build, ".prepare-lock") && nsw_component(manifest->system) &&
		   nsw_component(manifest->recovery) && !manifest_equal(manifest->system, manifest->recovery) &&
		   !manifest_equal(manifest->system, "BuildManifest.plist") &&
		   !manifest_equal(manifest->recovery, "BuildManifest.plist") && !manifest_equal(manifest->system, "Ready") &&
		   !manifest_equal(manifest->recovery, "Ready");
}

bool
nsw_hash_decode(const char *text, uint8_t digest[32])
{
	const char *digits = "0123456789abcdef";
	if (manifest_length(text) != 64) {
		return false;
	}
	for (uint32_t index = 0; index < 64; index++) {
		uint32_t value = 0;
		while (value < 16 && digits[value] != text[index]) {
			value++;
		}
		if (value == 16) {
			return false;
		}
		if ((index & 1) == 0) {
			digest[index / 2] = value << 4;
		} else {
			digest[index / 2] |= value;
		}
	}
	return true;
}

bool
nsw_request_parse(const char *data, uint32_t length, nsw_request_t *request)
{
	char hash[65];
	char *fields[] = {request->build, request->transaction, hash};
	uint32_t capacities[] = {64, 96, 65};
	uint32_t offset = 0;
	for (uint32_t field = 0; field < 3; field++) {
		uint32_t size = 0;
		while (offset < length && data[offset] != '\n') {
			if (size + 1 >= capacities[field] || data[offset] == 0) {
				return false;
			}
			fields[field][size++] = data[offset++];
		}
		if (offset == length || size == 0) {
			return false;
		}
		fields[field][size] = 0;
		offset++;
	}
	if (offset != length || !nsw_component(request->build) || !nsw_component(request->transaction)) {
		return false;
	}
	uint32_t prefix = manifest_length(request->build);
	if (manifest_length(request->transaction) <= prefix + 1) {
		return false;
	}
	for (uint32_t index = 0; index < prefix; index++) {
		if (request->transaction[index] != request->build[index]) {
			return false;
		}
	}
	return manifest_length(request->transaction) > prefix + 1 && request->transaction[prefix] == '-' &&
		   nsw_hash_decode(hash, request->manifest_hash);
}
