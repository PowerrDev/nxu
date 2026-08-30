#ifndef NXU_KERN_BOOT_MODE_H
#define NXU_KERN_BOOT_MODE_H

#include <stdbool.h>

typedef enum {
	BOOT_MODE_NORMAL = 0,
	BOOT_MODE_TRIAGE_OS = 1
} boot_mode_t;

void boot_mode_init(void);
bool boot_mode_poll(void);
boot_mode_t boot_mode_current(void);
bool boot_mode_is_triage_os(void);
const char *boot_mode_name(void);

#endif
