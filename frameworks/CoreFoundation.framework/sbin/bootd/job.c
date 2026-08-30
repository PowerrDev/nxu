/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/CoreFoundation.framework/sbin/bootd/job.c
 *
 * One bootd-managed process lifetime. Recovery-image selection, exit reaping
 * and crash throttling stay here so the manager only owns discovery and
 * activation policy.
 */

#include <frameworks/CoreFoundation.framework/sbin/bootd/job.h>

#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

#define BOOTD_COMPARE_BUFFER_SIZE 256U
#define BOOTD_CRASH_WINDOW_US 10000000ULL
#define BOOTD_CRASH_LIMIT 5U
#define BOOTD_THROTTLE_US 10000000ULL

typedef enum {
	BOOTD_IMAGE_COMPARE_UNAVAILABLE,
	BOOTD_IMAGE_COMPARE_EQUAL,
	BOOTD_IMAGE_COMPARE_DIFFERENT
} bootd_image_compare_t;

static void
bootd_job_log(const bootd_job_t *job, const char *message)
{
	(void)nxu_write(1U, "bootd: ", 7ULL);
	(void)nxu_write(1U, job->config.label, nxu_strlen(job->config.label));
	(void)nxu_write(1U, ": ", 2ULL);
	(void)nxu_write(1U, message, nxu_strlen(message));
	(void)nxu_write(1U, "\n", 1ULL);
}

static void
bootd_job_make_process_name(bootd_job_t *job)
{
	const char *program = job->config.program;
	uint64_t length = nxu_strlen(program);
	uint64_t start = length;

	while (start != 0ULL && program[start - 1ULL] != '/') start--;

	uint32_t written = 0U;
	while (program[start] != '\0' && written < BOOTD_PROCESS_NAME_MAX) {
		job->process_name[written++] = program[start++];
	}
	job->process_name[written] = '\0';

	if (written != 0U) return;

	uint64_t label_length = nxu_strlen(job->config.label);
	while (written < BOOTD_PROCESS_NAME_MAX && written < label_length) {
		job->process_name[written] = job->config.label[written];
		written++;
	}
	job->process_name[written] = '\0';
}

static bootd_image_compare_t
bootd_images_compare(const char *primary_path, const char *recovery_path)
{
	if (recovery_path[0] == '\0') return BOOTD_IMAGE_COMPARE_UNAVAILABLE;

	int64_t recovery = nxu_open(recovery_path, NXU_O_READ);
	if (recovery < 0) return BOOTD_IMAGE_COMPARE_UNAVAILABLE;

	int64_t primary = nxu_open(primary_path, NXU_O_READ);
	if (primary < 0) {
		(void)nxu_close((uint64_t)recovery);
		return BOOTD_IMAGE_COMPARE_DIFFERENT;
	}

	char primary_buffer[BOOTD_COMPARE_BUFFER_SIZE];
	char recovery_buffer[BOOTD_COMPARE_BUFFER_SIZE];
	bootd_image_compare_t result = BOOTD_IMAGE_COMPARE_EQUAL;

	for (;;) {
		int64_t primary_read = nxu_read((uint64_t)primary, primary_buffer, sizeof(primary_buffer));
		int64_t recovery_read = nxu_read((uint64_t)recovery, recovery_buffer, sizeof(recovery_buffer));

		if (primary_read < 0 || recovery_read < 0) {
			result = BOOTD_IMAGE_COMPARE_UNAVAILABLE;
			break;
		}

		if (primary_read != recovery_read) {
			result = BOOTD_IMAGE_COMPARE_DIFFERENT;
			break;
		}

		if (primary_read == 0) break;

		for (int64_t index = 0; index < primary_read; index++) {
			if (primary_buffer[index] == recovery_buffer[index]) continue;
			result = BOOTD_IMAGE_COMPARE_DIFFERENT;
			break;
		}

		if (result != BOOTD_IMAGE_COMPARE_EQUAL) break;
	}

	(void)nxu_close((uint64_t)primary);
	(void)nxu_close((uint64_t)recovery);
	return result;
}

static bool
bootd_job_spawn(bootd_job_t *job, uint64_t now_us)
{
	const char *path = job->config.program;
	bootd_image_compare_t comparison = bootd_images_compare(job->config.program, job->config.recovery_program);

	if (comparison == BOOTD_IMAGE_COMPARE_DIFFERENT && job->config.recovery_program[0] != '\0') {
		bootd_job_log(job, "primary image differs from recovery; using recovery");
		path = job->config.recovery_program;
	}

	int64_t pid = nxu_spawn(path, job->process_name);
	if (pid < 0 && path == job->config.program && job->config.recovery_program[0] != '\0') {
		bootd_job_log(job, "primary launch failed; trying recovery");
		pid = nxu_spawn(job->config.recovery_program, job->process_name);
	}

	if (pid < 0) {
		job->state = job->config.keep_alive ? BOOTD_JOB_THROTTLED : BOOTD_JOB_FAILED;
		job->restart_after_us = now_us + BOOTD_THROTTLE_US;
		bootd_job_log(job, "launch failed");
		return false;
	}

	job->pid = (uint64_t)pid;
	job->state = BOOTD_JOB_RUNNING;
	bootd_job_log(job, "started");
	return true;
}

static void
bootd_job_exited(bootd_job_t *job, uint64_t status, uint64_t now_us)
{
	job->pid = 0ULL;
	(void)status;
	bootd_job_log(job, "exited");

	if (!job->config.keep_alive || !job->activation_requested) {
		job->state = BOOTD_JOB_EXITED;
		return;
	}

	if (job->crash_window_start_us == 0ULL || now_us - job->crash_window_start_us > BOOTD_CRASH_WINDOW_US) {
		job->crash_window_start_us = now_us;
		job->crash_count = 1U;
	} else {
		job->crash_count++;
	}

	if (job->crash_count >= BOOTD_CRASH_LIMIT) {
		job->state = BOOTD_JOB_THROTTLED;
		job->restart_after_us = now_us + BOOTD_THROTTLE_US;
		job->crash_window_start_us = now_us;
		job->crash_count = 0U;
		bootd_job_log(job, "crash limit reached; restart throttled");
		return;
	}

	job->state = BOOTD_JOB_LOADED;
	job->restart_after_us = now_us;
}

bool
bootd_job_init(bootd_job_t *job, const service_config_t *config, const char *boot_args, bool safe_mode)
{
	if (job == 0 || config == 0 || boot_args == 0) return false;

	*job = (bootd_job_t) {
		.config = *config,
		.state = BOOTD_JOB_LOADED,
		.pid = 0ULL,
		.restart_after_us = 0ULL,
		.crash_window_start_us = 0ULL,
		.crash_count = 0U,
		.activation_requested = false
	};
	bootd_job_make_process_name(job);

	if (config->disabled) job->state = BOOTD_JOB_DISABLED;
	if (safe_mode && config->disabled_in_safe_mode) job->state = BOOTD_JOB_DISABLED;

	if (config->disabled_boot_argument[0] != '\0' && nxu_arg_present(boot_args, config->disabled_boot_argument)) {
		job->state = BOOTD_JOB_DISABLED;
	}

	return true;
}

void
bootd_job_activate(bootd_job_t *job)
{
	if (job == 0 || job->state == BOOTD_JOB_DISABLED) return;
	job->activation_requested = true;
}

void
bootd_job_poll(bootd_job_t *job, uint64_t now_us)
{
	if (job == 0 || job->state == BOOTD_JOB_DISABLED || !job->activation_requested) return;

	if (job->state == BOOTD_JOB_RUNNING) {
		uint64_t status = 0ULL;
		int64_t result = nxu_waitpid(job->pid, &status);
		if (result == -NXU_SYS_E_AGAIN) return;

		if (result < 0) {
			job->state = BOOTD_JOB_FAILED;
			bootd_job_log(job, "waitpid failed");
			return;
		}

		bootd_job_exited(job, status, now_us);
	}

	if (job->state == BOOTD_JOB_THROTTLED && now_us >= job->restart_after_us) job->state = BOOTD_JOB_LOADED;
	if (job->state != BOOTD_JOB_LOADED || now_us < job->restart_after_us) return;
	(void)bootd_job_spawn(job, now_us);
}

const char *
bootd_job_state_name(bootd_job_state_t state)
{
	switch (state) {
	case BOOTD_JOB_LOADED: return "loaded";
	case BOOTD_JOB_RUNNING: return "running";
	case BOOTD_JOB_THROTTLED: return "throttled";
	case BOOTD_JOB_EXITED: return "exited";
	case BOOTD_JOB_DISABLED: return "disabled";
	case BOOTD_JOB_FAILED: return "failed";
	default: return "unknown";
	}
}
