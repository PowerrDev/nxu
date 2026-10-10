#ifndef NXU_DESKTOP_SERVICE_H
#define NXU_DESKTOP_SERVICE_H
#include <CoreGraphics.h>

uint32_t DesktopServiceRun(const CoreGraphicsHostV5 *host);
bool DesktopServiceBeginSession(uint32_t scale, uint32_t width, uint32_t height, uint32_t menubar);
bool DesktopServiceGetTime(uint64_t *seconds);
void DesktopServiceShutdown(void);
uint32_t DesktopServiceScale(void);
uint32_t DesktopServiceGetActivity(void *, CoreGraphicsActivity *, CoreGraphicsProcessInfo *, uint32_t, uint32_t *);
uint32_t DesktopServiceListDirectory(void *, const char *, uint32_t, CoreGraphicsDirEntry *, uint32_t, uint32_t *, bool *);

#endif
