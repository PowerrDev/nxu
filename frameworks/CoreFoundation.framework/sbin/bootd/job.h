#ifndef NXU_BOOTD_JOB_H
#define NXU_BOOTD_JOB_H

#include <frameworks/CoreFoundation.framework/lib/service/service_config.h>

#include <stdbool.h>
#include <stdint.h>

#define BOOTD_PROCESS_NAME_MAX 31U

typedef enum {
	BOOTD_JOB_LOADED = 0,
	BOOTD_JOB_RUNNING,
	BOOTD_JOB_THROTTLED,
	BOOTD_JOB_EXITED,
	BOOTD_JOB_DISABLED,
	BOOTD_JOB_FAILED
} bootd_job_state_t;

typedef struct {
	service_config_t config;
	bootd_job_state_t state;
	uint64_t pid;
	uint64_t restart_after_us;
	uint64_t crash_window_start_us;
	uint32_t crash_count;
	bool activation_requested;
	char process_name[BOOTD_PROCESS_NAME_MAX + 1U];
} bootd_job_t;

bool bootd_job_init(bootd_job_t *job, const service_config_t *config, const char *boot_args, bool safe_mode);
void bootd_job_activate(bootd_job_t *job);
void bootd_job_poll(bootd_job_t *job, uint64_t now_us);
const char *bootd_job_state_name(bootd_job_state_t state);

#endif
