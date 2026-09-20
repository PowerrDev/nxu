#ifndef NXU_COREFOUNDATION_SERVICE_CONFIG_H
#define NXU_COREFOUNDATION_SERVICE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define SERVICE_LABEL_MAX 127U
#define SERVICE_PATH_MAX 255U
#define SERVICE_BOOT_ARGUMENT_MAX 63U
#define SERVICE_CONFIG_FILE_MAX 4096U

typedef struct {
	char label[SERVICE_LABEL_MAX + 1U];
	char program[SERVICE_PATH_MAX + 1U];
	char recovery_program[SERVICE_PATH_MAX + 1U];
	char disabled_boot_argument[SERVICE_BOOT_ARGUMENT_MAX + 1U];
	bool run_at_load;
	bool keep_alive;
	bool disabled;
	bool disabled_in_safe_mode;
	bool start_on_login;
	/* Capabilities bootd hands the job's process at spawn (default: none). */
	bool allow_filesystem_write;
	bool allow_display;
} service_config_t;

typedef enum {
	SERVICE_CONFIG_OK = 0,
	SERVICE_CONFIG_INVALID_ARGUMENT,
	SERVICE_CONFIG_NOT_FOUND,
	SERVICE_CONFIG_IO_ERROR,
	SERVICE_CONFIG_TOO_LARGE,
	SERVICE_CONFIG_BAD_PLIST,
	SERVICE_CONFIG_BAD_SCHEMA
} service_config_status_t;

service_config_status_t service_config_load(const char *path, service_config_t *config);
const char *service_config_status_name(service_config_status_t status);

#endif
