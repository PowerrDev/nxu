#ifndef NXU_BOOTD_MANAGER_H
#define NXU_BOOTD_MANAGER_H

#include <frameworks/CoreFoundation.framework/sbin/bootd/job.h>
#include <frameworks/CoreFoundation.framework/sbin/bootd/registry.h>

#include <stdbool.h>
#include <stdint.h>

#define BOOTD_MAX_JOBS 32U
#define BOOTD_BOOT_ARGS_SIZE 512U

typedef struct {
	bootd_job_t jobs[BOOTD_MAX_JOBS];
	uint32_t job_count;
	char boot_args[BOOTD_BOOT_ARGS_SIZE];
	bool safe_mode;
	bootd_registry_t registry;
} bootd_manager_t;

bool bootd_manager_init(bootd_manager_t *manager);
void bootd_manager_start(bootd_manager_t *manager);
void bootd_manager_poll(bootd_manager_t *manager);

#endif
