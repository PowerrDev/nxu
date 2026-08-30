#ifndef NXU_DRIVERS_VIDEO_DISPLAY_H
#define NXU_DRIVERS_VIDEO_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct display_device display_device_t;
typedef bool (*display_present_fn)(
	display_device_t *display,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height
);

struct display_device {
	uint32_t *framebuffer;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t bytes_per_pixel;

	void *driver;
	display_present_fn present;
	bool registered;
};

bool display_init(void);
bool display_register(display_device_t *display);
display_device_t *display_primary(void);

bool display_present(
	display_device_t *display,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height
);

bool display_present_full(display_device_t *display);
void display_dump(void);

#endif
