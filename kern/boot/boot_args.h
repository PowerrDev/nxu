#ifndef NXU_KERN_BOOT_BOOT_ARGS_H
#define NXU_KERN_BOOT_BOOT_ARGS_H

#include <stdbool.h>
#include <stdint.h>

#define BOOT_ARGS_MAX 511U

typedef enum {
	BOOT_COMPONENT_GPU,
	BOOT_COMPONENT_INPUT,
	BOOT_COMPONENT_BLOCK,
	BOOT_COMPONENT_SOUND,
	BOOT_COMPONENT_LOGD,
	BOOT_COMPONENT_PATCHD
} boot_component_t;

bool boot_args_init(void);
const char *boot_args_raw(void);
bool boot_arg_present(const char *argument);
/*
 * Add one argument to the boot arguments the kernel and its user processes see
 * (bootd reads them through the boot-args syscall). For a boot that decides
 * something the command line did not; false when it does not fit.
 */
bool boot_args_append(const char *argument);
bool boot_arg_value(const char *name, char *value, uint32_t capacity);
bool boot_arg_bool(const char *name, bool default_value);
uint32_t boot_arg_uint32(const char *name, uint32_t default_value);
bool boot_args_verbose(void);
bool boot_args_safe_mode(void);
bool boot_args_component_disabled(boot_component_t component);
void boot_args_dump(void);

#endif
