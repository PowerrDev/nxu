/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/CoreFoundation.framework/sbin/bootd/manager.c
 *
 * Discover declarative root-service jobs, validate their configuration and
 * activate jobs whose policy says they participate in the current boot.
 */

#include <frameworks/CoreFoundation.framework/sbin/bootd/manager.h>

#include <frameworks/CoreFoundation.framework/lib/service/service_config.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

#define BOOTD_SERVICE_DIRECTORY "/disk/System/Library/BootDaemons"
#define BOOTD_SERVICE_SUFFIX ".plist"
#define BOOTD_SERVICE_PATH_MAX 256U

static void
bootd_manager_log(const char *message)
{
	(void)nxu_write(1U, "bootd: ", 7ULL);
	(void)nxu_write(1U, message, nxu_strlen(message));
	(void)nxu_write(1U, "\n", 1ULL);
}

static void
bootd_manager_log_service(const char *prefix, const char *label)
{
	(void)nxu_write(1U, "bootd: ", 7ULL);
	(void)nxu_write(1U, prefix, nxu_strlen(prefix));
	(void)nxu_write(1U, label, nxu_strlen(label));
	(void)nxu_write(1U, "\n", 1ULL);
}

static bool
bootd_name_has_suffix(const char *name, const char *suffix)
{
	uint64_t name_length = nxu_strlen(name);
	uint64_t suffix_length = nxu_strlen(suffix);
	if (name_length < suffix_length) return false;

	uint64_t start = name_length - suffix_length;
	for (uint64_t index = 0ULL; index < suffix_length; index++) {
		if (name[start + index] != suffix[index]) return false;
	}
	return true;
}

static bool
bootd_service_path(char destination[BOOTD_SERVICE_PATH_MAX], const char *name)
{
	uint64_t directory_length = nxu_strlen(BOOTD_SERVICE_DIRECTORY);
	uint64_t name_length = nxu_strlen(name);
	if (directory_length + 1ULL + name_length + 1ULL > BOOTD_SERVICE_PATH_MAX) return false;

	uint64_t written = 0ULL;
	for (uint64_t index = 0ULL; index < directory_length; index++) destination[written++] = BOOTD_SERVICE_DIRECTORY[index];
	destination[written++] = '/';
	for (uint64_t index = 0ULL; index < name_length; index++) destination[written++] = name[index];
	destination[written] = '\0';
	return true;
}

static bool
bootd_manager_duplicate_label(const bootd_manager_t *manager, const char *label)
{
	for (uint32_t index = 0U; index < manager->job_count; index++) {
		if (nxu_streq(manager->jobs[index].config.label, label)) return true;
	}
	return false;
}

static bool
bootd_manager_add_config(bootd_manager_t *manager, const char *path)
{
	if (manager->job_count >= BOOTD_MAX_JOBS) {
		bootd_manager_log("service table full; ignoring remaining definitions");
		return false;
	}

	service_config_t config;
	service_config_status_t status = service_config_load(path, &config);
	if (status != SERVICE_CONFIG_OK) {
		bootd_manager_log("invalid service definition ignored");
		return true;
	}

	if (bootd_manager_duplicate_label(manager, config.label)) {
		bootd_manager_log_service("duplicate service label ignored: ", config.label);
		return true;
	}

	bootd_job_t *job = &manager->jobs[manager->job_count];
	if (!bootd_job_init(job, &config, manager->boot_args, manager->safe_mode)) return false;
	manager->job_count++;

	bootd_manager_log_service("loaded ", config.label);
	if (job->state == BOOTD_JOB_DISABLED) bootd_manager_log_service("disabled ", config.label);
	return true;
}

static bool
bootd_manager_discover(bootd_manager_t *manager)
{
	int64_t directory = nxu_open(BOOTD_SERVICE_DIRECTORY, NXU_O_READ);
	if (directory < 0) {
		bootd_manager_log("unable to open " BOOTD_SERVICE_DIRECTORY);
		return false;
	}

	bool success = true;
	for (;;) {
		nxu_dirent_t entry;
		int64_t result = nxu_readdir((uint64_t)directory, &entry);
		if (result < 0) {
			success = false;
			bootd_manager_log("directory enumeration failed");
			break;
		}
		if (result == 0) break;

		if (entry.type == NXU_DIRENT_TYPE_DIRECTORY) continue;
		if (!bootd_name_has_suffix(entry.name, BOOTD_SERVICE_SUFFIX)) continue;

		char path[BOOTD_SERVICE_PATH_MAX];
		if (!bootd_service_path(path, entry.name)) {
			bootd_manager_log("service definition path too long; ignored");
			continue;
		}

		if (!bootd_manager_add_config(manager, path)) {
			success = false;
			break;
		}
	}

	(void)nxu_close((uint64_t)directory);
	return success;
}

bool
bootd_manager_init(bootd_manager_t *manager)
{
	if (manager == 0) return false;
	*manager = (bootd_manager_t) { 0 };

	if (nxu_get_boot_args(manager->boot_args, sizeof(manager->boot_args)) < 0) manager->boot_args[0] = '\0';
	manager->safe_mode = nxu_arg_present(manager->boot_args, "-x");

	bootd_manager_log("loading service definitions");
	if (!bootd_manager_discover(manager)) return false;
	return true;
}

void
bootd_manager_start(bootd_manager_t *manager)
{
	if (manager == 0) return;

	for (uint32_t index = 0U; index < manager->job_count; index++) {
		bootd_job_t *job = &manager->jobs[index];
		if (!job->config.run_at_load) continue;
		bootd_job_activate(job);
	}

	bootd_manager_poll(manager);
}

void
bootd_manager_poll(bootd_manager_t *manager)
{
	if (manager == 0) return;

	int64_t uptime = nxu_uptime_us();
	if (uptime < 0) return;
	uint64_t now_us = (uint64_t)uptime;

	for (uint32_t index = 0U; index < manager->job_count; index++) bootd_job_poll(&manager->jobs[index], now_us);
}
