/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        skylight/skylight.c
 *
 * See skylight/skylight.h. Software rendering on the CPU, synchronous
 * presentation through the backend.
 */

#include <skylight/skylight.h>

#include <kern/console/console.h>
#include <kern/lock.h>
#include <kern/memory/heap.h>
#include <kern/sched_prism/sched.h>

#include <string.h>

#define SL_DEVICE_MAX 4U
#define SL_DISPLAY_MAX 4U
#define SL_NAME_MAX 32U

struct sl_device {
	char name[SL_NAME_MAX];
	const sl_backend_ops_t *ops;
	void *context;
	bool used;
};

struct sl_surface {
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	sl_format_t format;
	/* Allocated here (freed on destroy), or a backend's memory (a scanout). */
	bool owned;
	/* Being presented: its pixels are the device's source right now. */
	volatile uint32_t busy;
	nxu_spinlock_t lock;
	sl_rect_t damage;
};

struct sl_display {
	sl_device_t *device;
	uint32_t index;
	sl_surface_t surface;
	uint64_t presents;
	bool used;
};

static sl_device_t g_sl_devices[SL_DEVICE_MAX];
static sl_display_t g_sl_displays[SL_DISPLAY_MAX];
static nxu_spinlock_t g_sl_lock = NXU_SPINLOCK_INIT;

/* --- Rectangles ----------------------------------------------------------- */

sl_rect_t sl_rect_intersection(sl_rect_t a, sl_rect_t b)
{
	int64_t left = a.x > b.x ? a.x : b.x;
	int64_t top = a.y > b.y ? a.y : b.y;
	int64_t right_a = (int64_t)a.x + a.width;
	int64_t right_b = (int64_t)b.x + b.width;
	int64_t bottom_a = (int64_t)a.y + a.height;
	int64_t bottom_b = (int64_t)b.y + b.height;
	int64_t right = right_a < right_b ? right_a : right_b;
	int64_t bottom = bottom_a < bottom_b ? bottom_a : bottom_b;

	if (right <= left || bottom <= top) return (sl_rect_t) { 0 };
	return (sl_rect_t) { (int32_t)left, (int32_t)top, (uint32_t)(right - left), (uint32_t)(bottom - top) };
}

sl_rect_t sl_rect_union(sl_rect_t a, sl_rect_t b)
{
	if (sl_rect_empty(a)) return b;
	if (sl_rect_empty(b)) return a;

	int64_t left = a.x < b.x ? a.x : b.x;
	int64_t top = a.y < b.y ? a.y : b.y;
	int64_t right_a = (int64_t)a.x + a.width;
	int64_t right_b = (int64_t)b.x + b.width;
	int64_t bottom_a = (int64_t)a.y + a.height;
	int64_t bottom_b = (int64_t)b.y + b.height;
	int64_t right = right_a > right_b ? right_a : right_b;
	int64_t bottom = bottom_a > bottom_b ? bottom_a : bottom_b;

	return (sl_rect_t) { (int32_t)left, (int32_t)top, (uint32_t)(right - left), (uint32_t)(bottom - top) };
}

static sl_rect_t sl_surface_bounds(const sl_surface_t *surface)
{
	return (sl_rect_t) { 0, 0, surface->width, surface->height };
}

/* --- Backends -------------------------------------------------------------- */

bool sl_device_register(const char *name, const sl_backend_ops_t *ops, void *context, sl_device_t **device_out)
{
	if (name == 0 || ops == 0 || ops->present == 0 || device_out == 0) return false;

	sl_device_t *device = 0;

	nxu_spin_lock(&g_sl_lock);
	for (uint32_t index = 0U; index < SL_DEVICE_MAX; index++) {
		if (g_sl_devices[index].used) continue;
		device = &g_sl_devices[index];
		*device = (sl_device_t) { .ops = ops, .context = context, .used = true };
		size_t length = strlen(name);
		if (length >= SL_NAME_MAX) length = SL_NAME_MAX - 1U;
		memcpy(device->name, name, length);
		break;
	}
	nxu_spin_unlock(&g_sl_lock);

	if (device == 0) return false;
	kprintf("sl_device_register: %s\n", device->name);
	*device_out = device;
	return true;
}

bool sl_display_register(
	sl_device_t *device,
	uint32_t index,
	uint32_t *pixels,
	uint32_t width,
	uint32_t height,
	uint32_t stride,
	sl_display_t **display_out
)
{
	if (device == 0 || pixels == 0 || width == 0U || height == 0U || stride < width || display_out == 0) return false;

	sl_display_t *display = 0;

	nxu_spin_lock(&g_sl_lock);
	for (uint32_t slot = 0U; slot < SL_DISPLAY_MAX; slot++) {
		if (g_sl_displays[slot].used) continue;
		display = &g_sl_displays[slot];
		*display = (sl_display_t) {
			.device = device,
			.index = index,
			.surface = {
				.pixels = pixels,
				.width = width,
				.height = height,
				.stride = stride,
				.format = SL_FORMAT_XRGB8888,
				.owned = false,
				.lock = NXU_SPINLOCK_INIT
			},
			.used = true
		};
		break;
	}
	nxu_spin_unlock(&g_sl_lock);

	if (display == 0) return false;
	kprintf("sl_display_register: %s scanout %u, %ux%u\n", device->name, index, width, height);
	*display_out = display;
	return true;
}

/* --- Displays --------------------------------------------------------------- */

sl_display_t *sl_display_primary(void)
{
	for (uint32_t slot = 0U; slot < SL_DISPLAY_MAX; slot++) {
		if (g_sl_displays[slot].used) return &g_sl_displays[slot];
	}
	return 0;
}

uint32_t sl_display_width(const sl_display_t *display)
{
	return display != 0 ? display->surface.width : 0U;
}

uint32_t sl_display_height(const sl_display_t *display)
{
	return display != 0 ? display->surface.height : 0U;
}

sl_surface_t *sl_display_surface(sl_display_t *display)
{
	return display != 0 ? &display->surface : 0;
}

bool sl_present(sl_display_t *display, sl_rect_t rect)
{
	if (display == 0) return false;

	sl_surface_t *surface = &display->surface;
	sl_rect_t damage = sl_take_damage(surface);
	sl_rect_t area = sl_rect_intersection(sl_rect_empty(rect) ? damage : rect, sl_surface_bounds(surface));

	if (sl_rect_empty(area)) return true;

	__atomic_store_n(&surface->busy, 1U, __ATOMIC_RELEASE);
	bool ok = display->device->ops->present(display->device->context, display->index, area);
	__atomic_store_n(&surface->busy, 0U, __ATOMIC_RELEASE);

	display->presents++;
	return ok;
}

/* --- Surfaces ----------------------------------------------------------------- */

bool sl_surface_create(uint32_t width, uint32_t height, sl_format_t format, sl_surface_t **surface_out)
{
	if (width == 0U || height == 0U || width > 16384U || height > 16384U || surface_out == 0) return false;
	if (format != SL_FORMAT_XRGB8888 && format != SL_FORMAT_ARGB8888) return false;

	sl_surface_t *surface = kmalloc(sizeof(*surface));
	uint32_t *pixels = kcalloc((size_t)width * height, sizeof(uint32_t));

	if (surface == 0 || pixels == 0) {
		if (surface != 0) (void)kfree(surface);
		if (pixels != 0) (void)kfree(pixels);
		return false;
	}

	*surface = (sl_surface_t) {
		.pixels = pixels,
		.width = width,
		.height = height,
		.stride = width,
		.format = format,
		.owned = true,
		.lock = NXU_SPINLOCK_INIT
	};
	*surface_out = surface;
	return true;
}

void sl_surface_destroy(sl_surface_t *surface)
{
	if (surface == 0 || !surface->owned) return;

	while (__atomic_load_n(&surface->busy, __ATOMIC_ACQUIRE) != 0U) (void)sched_yield();
	(void)kfree(surface->pixels);
	(void)kfree(surface);
}

uint32_t sl_surface_width(const sl_surface_t *surface)
{
	return surface != 0 ? surface->width : 0U;
}

uint32_t sl_surface_height(const sl_surface_t *surface)
{
	return surface != 0 ? surface->height : 0U;
}

sl_format_t sl_surface_format(const sl_surface_t *surface)
{
	return surface != 0 ? surface->format : SL_FORMAT_XRGB8888;
}

uint32_t *sl_surface_map(sl_surface_t *surface, uint32_t *stride_out)
{
	if (surface == 0) return 0;

	/* Never hand out pixels the device is copying from. */
	while (__atomic_load_n(&surface->busy, __ATOMIC_ACQUIRE) != 0U) {
		if (!sched_yield()) __asm__ volatile("" ::: "memory");
	}

	if (stride_out != 0) *stride_out = surface->stride;
	return surface->pixels;
}

void sl_surface_unmap(sl_surface_t *surface)
{
	(void)surface;
}

void sl_damage(sl_surface_t *surface, sl_rect_t rect)
{
	if (surface == 0) return;

	sl_rect_t area = sl_rect_intersection(rect, sl_surface_bounds(surface));
	if (sl_rect_empty(area)) return;

	nxu_spin_lock(&surface->lock);
	surface->damage = sl_rect_union(surface->damage, area);
	nxu_spin_unlock(&surface->lock);
}

sl_rect_t sl_take_damage(sl_surface_t *surface)
{
	if (surface == 0) return (sl_rect_t) { 0 };

	nxu_spin_lock(&surface->lock);
	sl_rect_t damage = surface->damage;
	surface->damage = (sl_rect_t) { 0 };
	nxu_spin_unlock(&surface->lock);
	return damage;
}

/* --- Drawing ----------------------------------------------------------------- */

/* `over` (ARGB, straight alpha) over the opaque `under`. */
static uint32_t sl_blend_pixel(uint32_t under, uint32_t over)
{
	uint32_t alpha = over >> 24;

	if (alpha == 0U) return under;
	if (alpha == 255U) return over;

	uint32_t keep = 255U - alpha;
	uint32_t red = (((over >> 16) & 0xFFU) * alpha + ((under >> 16) & 0xFFU) * keep + 127U) / 255U;
	uint32_t green = (((over >> 8) & 0xFFU) * alpha + ((under >> 8) & 0xFFU) * keep + 127U) / 255U;
	uint32_t blue = ((over & 0xFFU) * alpha + (under & 0xFFU) * keep + 127U) / 255U;

	return 0xFF000000U | (red << 16) | (green << 8) | blue;
}

void sl_fill_rect(sl_surface_t *surface, sl_rect_t rect, uint32_t color, sl_blend_t blend)
{
	if (surface == 0) return;

	sl_rect_t area = sl_rect_intersection(rect, sl_surface_bounds(surface));
	if (sl_rect_empty(area)) return;

	uint32_t *pixels = sl_surface_map(surface, 0);
	bool blending = blend == SL_BLEND_OVER && (color >> 24) != 255U;

	for (uint32_t row = 0U; row < area.height; row++) {
		uint32_t *line = pixels + (size_t)(area.y + (int32_t)row) * surface->stride + area.x;

		for (uint32_t column = 0U; column < area.width; column++) {
			line[column] = blending ? sl_blend_pixel(line[column], color) : color;
		}
	}

	sl_surface_unmap(surface);
	sl_damage(surface, area);
}

void sl_copy(sl_surface_t *destination, int32_t x, int32_t y, sl_surface_t *source, sl_rect_t source_rect, sl_blend_t blend)
{
	if (destination == 0 || source == 0) return;

	sl_rect_t from = sl_rect_intersection(source_rect, sl_surface_bounds(source));
	if (sl_rect_empty(from)) return;

	/* Where `from` lands, clipped to the destination, and back. */
	sl_rect_t landed = { x + (from.x - source_rect.x), y + (from.y - source_rect.y), from.width, from.height };
	sl_rect_t to = sl_rect_intersection(landed, sl_surface_bounds(destination));
	if (sl_rect_empty(to)) return;

	int32_t source_x = from.x + (to.x - landed.x);
	int32_t source_y = from.y + (to.y - landed.y);
	uint32_t *source_pixels = sl_surface_map(source, 0);
	uint32_t *destination_pixels = sl_surface_map(destination, 0);
	bool blending = blend == SL_BLEND_OVER && source->format == SL_FORMAT_ARGB8888;

	for (uint32_t row = 0U; row < to.height; row++) {
		const uint32_t *in = source_pixels + (size_t)(source_y + (int32_t)row) * source->stride + source_x;
		uint32_t *out = destination_pixels + (size_t)(to.y + (int32_t)row) * destination->stride + to.x;

		if (!blending) {
			memmove(out, in, (size_t)to.width * sizeof(uint32_t));
			continue;
		}
		for (uint32_t column = 0U; column < to.width; column++) out[column] = sl_blend_pixel(out[column], in[column]);
	}

	sl_surface_unmap(destination);
	sl_surface_unmap(source);
	sl_damage(destination, to);
}

void sl_upload(sl_surface_t *destination, int32_t x, int32_t y, const uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride)
{
	if (destination == 0 || pixels == 0 || stride < width) return;

	sl_rect_t landed = { x, y, width, height };
	sl_rect_t to = sl_rect_intersection(landed, sl_surface_bounds(destination));
	if (sl_rect_empty(to)) return;

	uint32_t *destination_pixels = sl_surface_map(destination, 0);

	for (uint32_t row = 0U; row < to.height; row++) {
		const uint32_t *in = pixels + (size_t)(to.y - y + (int32_t)row) * stride + (to.x - x);
		memcpy(destination_pixels + (size_t)(to.y + (int32_t)row) * destination->stride + to.x, in, (size_t)to.width * sizeof(uint32_t));
	}

	sl_surface_unmap(destination);
	sl_damage(destination, to);
}

void sl_dump(void)
{
	for (uint32_t slot = 0U; slot < SL_DISPLAY_MAX; slot++) {
		const sl_display_t *display = &g_sl_displays[slot];
		if (!display->used) continue;
		kprintf("sl_dump: %s scanout %u %ux%u, %llu present(s)\n", display->device->name, display->index,
			display->surface.width, display->surface.height, (unsigned long long)display->presents);
	}
}
