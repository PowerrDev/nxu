#ifndef NXU_KERN_BOOTLOG_H
#define NXU_KERN_BOOTLOG_H

#include <platform/dtb.h>
#include <platform/platform.h>

#include <stdint.h>

void nxu_boot_log_early(const void *dtb_address, uint64_t current_el_raw, uint32_t current_el);
void nxu_boot_log_platform(const dtb_t *dtb, const platform_t *platform);
void nxu_boot_log_higher_half(void);
void nxu_boot_log_driver_handoff(void);
void nxu_boot_log_ui_handoff(void);

#endif
