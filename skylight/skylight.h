#ifndef NXU_SKYLIGHT_SKYLIGHT_H
#define NXU_SKYLIGHT_SKYLIGHT_H

/*
 * Skylight: NXU's graphics API.
 *
 * Skylight sits between whoever draws (WindowServer, UIService, the apps'
 * frames) and the graphics driver, and only knows graphics concepts: a
 * device, the displays (scanouts) it drives, surfaces of pixels, rectangles,
 * fills and copies between surfaces, damage, and presenting a surface's
 * damage on a display. It knows nothing of windows, buttons, fonts, the Dock
 * or input: WindowServer decides "Voyager is at (120, 80), this region needs
 * recomposition"; Skylight performs the graphics operations and presents the
 * result; the backend (VirtIOGPUFamily first) gets it onto its device.
 *
 * This first version renders in software on the CPU and presents
 * synchronously. The shape leaves room for command buffers, asynchronous
 * submission with fences, textures and GPU acceleration, and more backends:
 * callers already go through surfaces and sl_present rather than a
 * framebuffer pointer and a driver call.
 *
 * Synchronization: a display's scanout surface is busy while it is being
 * presented (its pixels are the device's copy source), and sl_surface_map
 * waits for that to end, so a buffer is never modified mid-present.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	/* 32-bit pixels, 0x00RRGGBB; the top byte is ignored. */
	SL_FORMAT_XRGB8888 = 1,
	/* 32-bit pixels, 0xAARRGGBB, straight (not premultiplied) alpha. */
	SL_FORMAT_ARGB8888 = 2
} sl_format_t;

typedef enum {
	/* Copy source pixels as they are. */
	SL_BLEND_NONE = 0,
	/* Source (ARGB) over destination. */
	SL_BLEND_OVER = 1
} sl_blend_t;

typedef struct {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
} sl_rect_t;

typedef struct sl_device sl_device_t;
typedef struct sl_display sl_display_t;
typedef struct sl_surface sl_surface_t;

/*
 * The backend interface: what a graphics driver implements. A display's
 * scanout surface wraps memory the backend already owns (the device's scanout
 * backing); presenting tells the device a rectangle of it changed.
 */
typedef struct {
	/* Show `rect` of display `index`'s scanout surface. Returns when the device has it. */
	bool (*present)(void *context, uint32_t index, sl_rect_t rect);
} sl_backend_ops_t;

/* --- Backends ------------------------------------------------------------ */

bool sl_device_register(const char *name, const sl_backend_ops_t *ops, void *context, sl_device_t **device_out);

/* A scanout of `device`: `pixels` (XRGB8888, `stride` pixels apart) is what it shows. */
bool sl_display_register(
	sl_device_t *device,
	uint32_t index,
	uint32_t *pixels,
	uint32_t width,
	uint32_t height,
	uint32_t stride,
	sl_display_t **display_out
);

/* --- Devices and displays ------------------------------------------------ */

/* The first display registered, or 0. */
sl_display_t *sl_display_primary(void);
uint32_t sl_display_width(const sl_display_t *display);
uint32_t sl_display_height(const sl_display_t *display);

/* The surface whose pixels the display shows. */
sl_surface_t *sl_display_surface(sl_display_t *display);

/*
 * Present `rect` of the display's scanout surface (clipped to it), or its
 * accumulated damage when `rect` is empty. The surface is busy meanwhile.
 */
bool sl_present(sl_display_t *display, sl_rect_t rect);

/* --- Surfaces ------------------------------------------------------------ */

bool sl_surface_create(uint32_t width, uint32_t height, sl_format_t format, sl_surface_t **surface_out);
void sl_surface_destroy(sl_surface_t *surface);

uint32_t sl_surface_width(const sl_surface_t *surface);
uint32_t sl_surface_height(const sl_surface_t *surface);
sl_format_t sl_surface_format(const sl_surface_t *surface);

/*
 * CPU access for software rendering: the pixels and their stride. Waits
 * while the surface is being presented. Pair with sl_surface_unmap.
 */
uint32_t *sl_surface_map(sl_surface_t *surface, uint32_t *stride_out);
void sl_surface_unmap(sl_surface_t *surface);

/* Fill `rect` with `color` (ARGB; SL_BLEND_OVER blends it when not opaque). */
void sl_fill_rect(sl_surface_t *surface, sl_rect_t rect, uint32_t color, sl_blend_t blend);

/*
 * Copy `source_rect` of `source` to (`x`, `y`) of `destination`, clipped to
 * both. Records the destination rectangle as damage.
 */
void sl_copy(sl_surface_t *destination, int32_t x, int32_t y, sl_surface_t *source, sl_rect_t source_rect, sl_blend_t blend);

/* Copy raw pixels (a caller's buffer, `stride` pixels apart) into a surface. */
void sl_upload(sl_surface_t *destination, int32_t x, int32_t y, const uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride);

/* Damage: what changed since it was last taken. */
void sl_damage(sl_surface_t *surface, sl_rect_t rect);
sl_rect_t sl_take_damage(sl_surface_t *surface);

/* --- Rectangles ---------------------------------------------------------- */

static inline bool sl_rect_empty(sl_rect_t rect)
{
	return rect.width == 0U || rect.height == 0U;
}

sl_rect_t sl_rect_intersection(sl_rect_t a, sl_rect_t b);
sl_rect_t sl_rect_union(sl_rect_t a, sl_rect_t b);

void sl_dump(void);

#endif
