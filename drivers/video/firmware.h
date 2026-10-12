#ifndef NXU_FIRMWARE_DISPLAY_H
#define NXU_FIRMWARE_DISPLAY_H

#include <kern/i386/boot_info.h>

bool firmware_display_init(const i386_boot_info_t *boot);
#endif
