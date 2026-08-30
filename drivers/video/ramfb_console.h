#ifndef NXU_DRIVERS_VIDEO_RAMFB_CONSOLE_H
#define NXU_DRIVERS_VIDEO_RAMFB_CONSOLE_H

#include <platform/platform.h>

#include <stdbool.h>
#include <stdint.h>

bool ramfb_console_init(const platform_t *platform);
bool ramfb_console_available(void);
bool ramfb_console_set_mirroring(bool enabled);
uint32_t *ramfb_console_framebuffer(void);
uint32_t ramfb_console_width(void);
uint32_t ramfb_console_height(void);
uint32_t ramfb_console_stride(void);

#endif
