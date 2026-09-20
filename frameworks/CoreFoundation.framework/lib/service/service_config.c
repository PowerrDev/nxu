/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/CoreFoundation.framework/lib/service/service_config.c
 *
 * Translate one property-list service definition into bootd's stable job
 * configuration schema. Plist syntax remains a CoreFoundation concern;
 * lifecycle policy remains a bootd concern.
 */

#include <frameworks/CoreFoundation.framework/lib/service/service_config.h>

#include <frameworks/CoreFoundation.framework/lib/plist/plist.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

static bool
service_copy(char *destination, uint64_t capacity, const char *source)
{
	uint64_t length = nxu_strlen(source);
	if (length + 1ULL > capacity) return false;

	for (uint64_t index = 0ULL; index <= length; index++) destination[index] = source[index];
	return true;
}

static service_config_status_t
service_read_file(const char *path, char buffer[SERVICE_CONFIG_FILE_MAX + 1U], uint64_t *length)
{
	int64_t descriptor = nxu_open(path, NXU_O_READ);
	if (descriptor == -NXU_SYS_E_NOT_FOUND) return SERVICE_CONFIG_NOT_FOUND;
	if (descriptor < 0) return SERVICE_CONFIG_IO_ERROR;

	uint64_t complete = 0ULL;
	while (complete < SERVICE_CONFIG_FILE_MAX) {
		int64_t amount = nxu_read((uint64_t)descriptor, buffer + complete, SERVICE_CONFIG_FILE_MAX - complete);
		if (amount < 0) {
			(void)nxu_close((uint64_t)descriptor);
			return SERVICE_CONFIG_IO_ERROR;
		}

		if (amount == 0) break;
		complete += (uint64_t)amount;
	}

	if (complete == SERVICE_CONFIG_FILE_MAX) {
		char extra;
		int64_t amount = nxu_read((uint64_t)descriptor, &extra, 1ULL);
		if (amount != 0) {
			(void)nxu_close((uint64_t)descriptor);
			return amount < 0 ? SERVICE_CONFIG_IO_ERROR : SERVICE_CONFIG_TOO_LARGE;
		}
	}

	(void)nxu_close((uint64_t)descriptor);
	buffer[complete] = '\0';
	*length = complete;
	return SERVICE_CONFIG_OK;
}

static bool
service_key_is(const plist_event_t *key, const char *value)
{
	return key->type == PLIST_EVENT_KEY && nxu_streq(key->text, value);
}

static plist_status_t
service_skip_value(plist_parser_t *parser, const plist_event_t *value)
{
	if (value->type != PLIST_EVENT_DICT_BEGIN && value->type != PLIST_EVENT_ARRAY_BEGIN) return PLIST_STATUS_OK;

	uint32_t depth = 1U;
	while (depth != 0U) {
		plist_event_t event;
		plist_status_t status = plist_parser_next(parser, &event);
		if (status != PLIST_STATUS_OK) return status;

		if (event.type == PLIST_EVENT_DICT_BEGIN || event.type == PLIST_EVENT_ARRAY_BEGIN) depth++;
		if (event.type == PLIST_EVENT_DICT_END || event.type == PLIST_EVENT_ARRAY_END) depth--;
	}

	return PLIST_STATUS_OK;
}

static bool
service_apply_value(service_config_t *config, const plist_event_t *key, const plist_event_t *value)
{
	if (service_key_is(key, "Label")) {
		return value->type == PLIST_EVENT_STRING && service_copy(config->label, sizeof(config->label), value->text);
	}

	if (service_key_is(key, "Program")) {
		return value->type == PLIST_EVENT_STRING && service_copy(config->program, sizeof(config->program), value->text);
	}

	if (service_key_is(key, "RecoveryProgram")) {
		return value->type == PLIST_EVENT_STRING && service_copy(config->recovery_program, sizeof(config->recovery_program), value->text);
	}

	if (service_key_is(key, "DisabledBootArgument")) {
		return value->type == PLIST_EVENT_STRING && service_copy(
			config->disabled_boot_argument,
			sizeof(config->disabled_boot_argument),
			value->text
		);
	}

	if (service_key_is(key, "RunAtLoad")) {
		if (value->type != PLIST_EVENT_BOOLEAN) return false;
		config->run_at_load = value->boolean;
		return true;
	}

	if (service_key_is(key, "KeepAlive")) {
		if (value->type != PLIST_EVENT_BOOLEAN) return false;
		config->keep_alive = value->boolean;
		return true;
	}

	if (service_key_is(key, "Disabled")) {
		if (value->type != PLIST_EVENT_BOOLEAN) return false;
		config->disabled = value->boolean;
		return true;
	}

	if (service_key_is(key, "DisabledInSafeMode")) {
		if (value->type != PLIST_EVENT_BOOLEAN) return false;
		config->disabled_in_safe_mode = value->boolean;
		return true;
	}

	if (service_key_is(key, "StartOnLogin")) {
		if (value->type != PLIST_EVENT_BOOLEAN) return false;
		config->start_on_login = value->boolean;
		return true;
	}

	if (service_key_is(key, "AllowFilesystemWrite")) {
		if (value->type != PLIST_EVENT_BOOLEAN) return false;
		config->allow_filesystem_write = value->boolean;
		return true;
	}

	if (service_key_is(key, "AllowDisplay")) {
		if (value->type != PLIST_EVENT_BOOLEAN) return false;
		config->allow_display = value->boolean;
		return true;
	}

	return true;
}

static service_config_status_t
service_parse(const char *data, uint64_t length, service_config_t *config)
{
	plist_parser_t parser;
	plist_status_t status = plist_parser_init(&parser, data, length);
	if (status != PLIST_STATUS_OK) return SERVICE_CONFIG_BAD_PLIST;

	plist_event_t event;
	status = plist_parser_next(&parser, &event);
	if (status != PLIST_STATUS_OK || event.type != PLIST_EVENT_DICT_BEGIN) return SERVICE_CONFIG_BAD_SCHEMA;

	for (;;) {
		plist_event_t key;
		status = plist_parser_next(&parser, &key);
		if (status != PLIST_STATUS_OK) return SERVICE_CONFIG_BAD_PLIST;
		if (key.type == PLIST_EVENT_DICT_END) break;
		if (key.type != PLIST_EVENT_KEY) return SERVICE_CONFIG_BAD_SCHEMA;

		plist_event_t value;
		status = plist_parser_next(&parser, &value);
		if (status != PLIST_STATUS_OK) return SERVICE_CONFIG_BAD_PLIST;

		bool recognized = service_key_is(&key, "Label")
			|| service_key_is(&key, "Program")
			|| service_key_is(&key, "RecoveryProgram")
			|| service_key_is(&key, "DisabledBootArgument")
			|| service_key_is(&key, "RunAtLoad")
			|| service_key_is(&key, "KeepAlive")
			|| service_key_is(&key, "Disabled")
			|| service_key_is(&key, "DisabledInSafeMode")
			|| service_key_is(&key, "StartOnLogin")
			|| service_key_is(&key, "AllowFilesystemWrite")
			|| service_key_is(&key, "AllowDisplay");

		if (recognized) {
			if (!service_apply_value(config, &key, &value)) return SERVICE_CONFIG_BAD_SCHEMA;
		} else {
			status = service_skip_value(&parser, &value);
			if (status != PLIST_STATUS_OK) return SERVICE_CONFIG_BAD_PLIST;
		}
	}

	status = plist_parser_next(&parser, &event);
	if (status != PLIST_STATUS_END) return SERVICE_CONFIG_BAD_SCHEMA;
	if (config->label[0] == '\0' || config->program[0] == '\0') return SERVICE_CONFIG_BAD_SCHEMA;
	return SERVICE_CONFIG_OK;
}

service_config_status_t
service_config_load(const char *path, service_config_t *config)
{
	if (path == 0 || config == 0) return SERVICE_CONFIG_INVALID_ARGUMENT;
	*config = (service_config_t) { 0 };

	char buffer[SERVICE_CONFIG_FILE_MAX + 1U];
	uint64_t length;
	service_config_status_t status = service_read_file(path, buffer, &length);
	if (status != SERVICE_CONFIG_OK) return status;
	if (length == 0ULL) return SERVICE_CONFIG_BAD_PLIST;

	return service_parse(buffer, length, config);
}

const char *
service_config_status_name(service_config_status_t status)
{
	switch (status) {
	case SERVICE_CONFIG_OK: return "ok";
	case SERVICE_CONFIG_INVALID_ARGUMENT: return "invalid argument";
	case SERVICE_CONFIG_NOT_FOUND: return "not found";
	case SERVICE_CONFIG_IO_ERROR: return "I/O error";
	case SERVICE_CONFIG_TOO_LARGE: return "configuration too large";
	case SERVICE_CONFIG_BAD_PLIST: return "invalid plist";
	case SERVICE_CONFIG_BAD_SCHEMA: return "invalid service schema";
	default: return "unknown service configuration error";
	}
}
