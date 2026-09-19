#include <drivers/video/ui_service_host.h>

#include <mach/arm64/timer.h>
#include <drivers/input/mouse.h>
#include <drivers/video/display.h>
#include <drivers/video/ramfb_console.h>
#include <drivers/virtio/virtio_input.h>
#include <kern/console/console.h>
#include <kern/memory/heap.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(NXU_UI_SERVICE)
#include <UIService.h>
#endif

#define UI_SERVICE_FRAME_INTERVAL_US 16667ULL

#if defined(NXU_UI_SERVICE)
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t *output_pixels;
	uint32_t *backbuffer;
	display_device_t *display;
	uint64_t last_present_us;
	int32_t pointer_x;
	int32_t pointer_y;
	int64_t raw_x;
	int64_t raw_y;
	uint64_t packet_count;
	uint32_t buttons;
	uint32_t pending_down;
	uint32_t pending_up;
	bool pending_move;
} UIServiceContext;

/*
 * Return UI's software backbuffer instead of the scanout memory itself.
 * Rendering a complete scene offscreen prevents users from seeing the old
 * damage region get erased before the replacement window/cursor is drawn.
 */
static uint32_t UIServiceGetSurface(void *opaque, UIServiceSurfaceDescriptor *surface)
{
	if (opaque == 0 || surface == 0) return UI_SERVICE_STATUS_INVALID_ARGUMENT;

	UIServiceContext *context = (UIServiceContext *)opaque;
	if (context->backbuffer == 0) return UI_SERVICE_STATUS_NO_SURFACE;

	*surface = (UIServiceSurfaceDescriptor) {
		.pixels = context->backbuffer,
		.width = context->width,
		.height = context->height,
		.stride_pixels = context->stride,
		.pixel_format = UI_SERVICE_PIXEL_FORMAT_XRGB8888
	};
	return UI_SERVICE_STATUS_OK;
}

/*
 * Pace software presents near 60 Hz. RAMFB has no real vblank handshake, so
 * this cannot provide hardware VSYNC, but it avoids flooding Cocoa with many
 * partially overlapping updates while a window is being dragged.
 */
static void UIServicePaceFrame(UIServiceContext *context)
{
	uint64_t now = timer_get_microseconds();
	if (now == 0ULL) return;

	if (context->last_present_us != 0ULL) {
		uint64_t elapsed = now - context->last_present_us;
		while (elapsed < UI_SERVICE_FRAME_INTERVAL_US) {
			__asm__ volatile("yield");
			now = timer_get_microseconds();
			elapsed = now - context->last_present_us;
		}
	}

	context->last_present_us = now;
}

/*
 * Copy one completed damage rectangle from UI's backbuffer to the actual NXU
 * scanout. This is the software equivalent of presenting a finished frame.
 */
static bool UIServiceCopyDamage(UIServiceContext *context, const UIServiceDamageRect *damage)
{
	if (context == 0 || damage == 0 || context->output_pixels == 0 || context->backbuffer == 0) return false;

	if (damage->x < 0 || damage->y < 0 || damage->width == 0U || damage->height == 0U) return false;

	uint32_t x = (uint32_t)damage->x;
	uint32_t y = (uint32_t)damage->y;
	if (x >= context->width || y >= context->height) return false;

	if (damage->width > context->width - x || damage->height > context->height - y) return false;

	for (uint32_t row = 0U; row < damage->height; row++) {
		uint32_t *destination = context->output_pixels + (uint64_t)(y + row) * context->stride + x;
		const uint32_t *source = context->backbuffer + (uint64_t)(y + row) * context->stride + x;
		memcpy(destination, source, (size_t)damage->width * sizeof(uint32_t));
	}

	return true;
}

/*
 * Present a finished UI frame. VirtIO GPU scanouts receive their transfer and
 * flush command after the backbuffer copy; RAMFB simply observes the completed
 * memory update on QEMU's next host refresh.
 */
static uint32_t UIServicePresent(void *opaque, const UIServiceDamageRect *damage)
{
	if (opaque == 0) return UI_SERVICE_STATUS_INVALID_ARGUMENT;

	UIServiceContext *context = (UIServiceContext *)opaque;
	UIServiceDamageRect full = {
		.x = 0,
		.y = 0,
		.width = context->width,
		.height = context->height
	};
	const UIServiceDamageRect *present_damage = damage == 0 ? &full : damage;

	UIServicePaceFrame(context);
	if (!UIServiceCopyDamage(context, present_damage)) return UI_SERVICE_STATUS_INVALID_ARGUMENT;

	if (context->display == 0) return UI_SERVICE_STATUS_OK;

	return display_present(
		context->display,
		(uint32_t)present_damage->x,
		(uint32_t)present_damage->y,
		present_damage->width,
		present_damage->height
	) ? UI_SERVICE_STATUS_OK : UI_SERVICE_STATUS_PRESENT_FAILED;
}

/*
 * Clamp a relative pointer accumulation to the visible surface.
 */
static int32_t UIServiceClampPointer(int64_t value, uint32_t extent)
{
	if (extent == 0U || value < 0) return 0;

	if ((uint64_t)value >= (uint64_t)extent) return (int32_t)(extent - 1U);
	return (int32_t)value;
}

/*
 * Translate NXU's relative VirtIO mouse state into a stable absolute UI point.
 */
static void UIServiceRefreshPointer(UIServiceContext *context)
{
	uint64_t packets = mouse_packet_count();
	if (packets == context->packet_count) return;

	int64_t raw_x = mouse_x();
	int64_t raw_y = mouse_y();
	int64_t next_x = (int64_t)context->pointer_x + (raw_x - context->raw_x);
	int64_t next_y = (int64_t)context->pointer_y + (raw_y - context->raw_y);
	int32_t pointer_x = UIServiceClampPointer(next_x, context->width);
	int32_t pointer_y = UIServiceClampPointer(next_y, context->height);
	uint32_t buttons = mouse_buttons();
	uint32_t changed = buttons ^ context->buttons;

	if (pointer_x != context->pointer_x || pointer_y != context->pointer_y) {
		context->pending_move = true;
	}

	context->pending_down |= changed & buttons;
	context->pending_up |= changed & context->buttons;
	context->pointer_x = pointer_x;
	context->pointer_y = pointer_y;
	context->raw_x = raw_x;
	context->raw_y = raw_y;
	context->packet_count = packets;
	context->buttons = buttons;
}

/*
 * Convert NXU mouse button bits into UI's platform-neutral ABI values.
 */
static uint32_t UIServiceButtonFromMask(uint32_t mask)
{
	if ((mask & MOUSE_BUTTON_LEFT) != 0U) return UI_SERVICE_POINTER_BUTTON_PRIMARY;

	if ((mask & MOUSE_BUTTON_RIGHT) != 0U) return UI_SERVICE_POINTER_BUTTON_SECONDARY;
	if ((mask & MOUSE_BUTTON_MIDDLE) != 0U) return UI_SERVICE_POINTER_BUTTON_MIDDLE;
	return UI_SERVICE_POINTER_BUTTON_NONE;
}

static uint32_t UIServiceSelectButtonMask(uint32_t mask)
{
	if ((mask & MOUSE_BUTTON_LEFT) != 0U) return MOUSE_BUTTON_LEFT;

	if ((mask & MOUSE_BUTTON_RIGHT) != 0U) return MOUSE_BUTTON_RIGHT;
	if ((mask & MOUSE_BUTTON_MIDDLE) != 0U) return MOUSE_BUTTON_MIDDLE;
	return 0U;
}

/*
 * Drain VirtIO Input directly as an early-boot fallback and return at most one
 * normalized event per call. IRQ-driven delivery remains enabled as well.
 */
static uint32_t UIServicePollEvent(void *opaque, UIServiceHostEvent *event)
{
	if (opaque == 0 || event == 0) return UI_SERVICE_STATUS_INVALID_ARGUMENT;

	UIServiceContext *context = (UIServiceContext *)opaque;
	virtio_input_service();
	UIServiceRefreshPointer(context);

	*event = (UIServiceHostEvent) {
		.event_type = UI_SERVICE_EVENT_NONE,
		.struct_size = sizeof(UIServiceHostEvent),
		.x = context->pointer_x,
		.y = context->pointer_y,
		.button = UI_SERVICE_POINTER_BUTTON_NONE,
		.reserved = 0U
	};

	if (context->pending_move) {
		context->pending_move = false;
		event->event_type = UI_SERVICE_EVENT_POINTER_MOVED;
		return UI_SERVICE_STATUS_OK;
	}

	uint32_t button_mask = UIServiceSelectButtonMask(context->pending_down);
	if (button_mask != 0U) {
		context->pending_down &= ~button_mask;
		event->event_type = UI_SERVICE_EVENT_POINTER_DOWN;
		event->button = UIServiceButtonFromMask(button_mask);
		return UI_SERVICE_STATUS_OK;
	}

	button_mask = UIServiceSelectButtonMask(context->pending_up);
	if (button_mask != 0U) {
		context->pending_up &= ~button_mask;
		event->event_type = UI_SERVICE_EVENT_POINTER_UP;
		event->button = UIServiceButtonFromMask(button_mask);
	}

	return UI_SERVICE_STATUS_OK;
}
#endif

bool ui_service_bootstrap(void)
{
#if !defined(NXU_UI_SERVICE)
	kputln("[com.butterscotch.uiservice]: NXU bridge is not linked");
	return false;
#else
	uint32_t *framebuffer = 0;
	uint32_t width = 0U;
	uint32_t height = 0U;
	uint32_t stride = 0U;
	display_device_t *display = display_primary();
	bool using_ramfb = false;

	/*
	 * Prefer an attached display device. RAMFB remains the emergency scanout
	 * when VirtIO GPU is unavailable during early UI bring-up.
	 */
	if (display != 0) {
		framebuffer = display->framebuffer;
		width = display->width;
		height = display->height;
		stride = display->stride;
	} else if (ramfb_console_available()) {
		using_ramfb = true;
		framebuffer = ramfb_console_framebuffer();
		width = ramfb_console_width();
		height = ramfb_console_height();
		stride = ramfb_console_stride();
	}

	if (framebuffer == 0 || width == 0U || height == 0U || stride < width) {
		kputln("[com.butterscotch.uiservice]: no scanout surface is available");
		return false;
	}

	if (UIServiceAPIVersion() != UI_SERVICE_API_VERSION) {
		kprintf("[com.butterscotch.uiservice]: API mismatch, kernel expects %u but framework reported %u\n", UI_SERVICE_API_VERSION, UIServiceAPIVersion());
		return false;
	}

	size_t pixel_count = (size_t)stride * (size_t)height;
	if (height != 0U && pixel_count / (size_t)height != (size_t)stride) {
		kputln("[com.butterscotch.uiservice]: framebuffer size overflow while allocating backbuffer");
		return false;
	}

	uint32_t *backbuffer = kcalloc(pixel_count, sizeof(uint32_t));
	if (backbuffer == 0) {
		kputln("[com.butterscotch.uiservice]: software backbuffer allocation failed");
		return false;
	}
	memcpy(backbuffer, framebuffer, pixel_count * sizeof(uint32_t));

	UIServiceContext context = {
		.width = width,
		.height = height,
		.stride = stride,
		.output_pixels = framebuffer,
		.backbuffer = backbuffer,
		.display = display,
		.last_present_us = 0ULL,
		.pointer_x = (int32_t)(width / 2U),
		.pointer_y = (int32_t)(height / 2U),
		.raw_x = mouse_x(),
		.raw_y = mouse_y(),
		.packet_count = mouse_packet_count(),
		.buttons = mouse_buttons(),
		.pending_down = 0U,
		.pending_up = 0U,
		.pending_move = false
	};

	UIServiceHostV2 host = {
		.header = {
			.struct_size = sizeof(UIServiceHostV2),
			.abi_version = UI_SERVICE_ABI_VERSION_V2
		},
		.capabilities = UI_SERVICE_HOST_CAPABILITIES_V2,
		.context = &context,
		.get_surface = UIServiceGetSurface,
		.present = UIServicePresent,
		.poll_event = UIServicePollEvent
	};

	if (UIServiceValidateHostV2(&host) != UI_SERVICE_STATUS_OK) {
		kputln("[com.butterscotch.uiservice]: NXU host ABI v2 rejected");
		(void)kfree(backbuffer);
		return false;
	}

	kprintf("[com.butterscotch.uiservice]: ABI v%u, %ux%u XRGB8888, %s\n", UIServiceABIVersion(), width, height, using_ramfb ? "RAMFB" : "VirtIO GPU");
	kputln(UIServiceHasInter() != 0U ? "[com.butterscotch.uiservice]: font renderer active" : "[com.butterscotch.uiservice]: bootstrap text renderer active");
	if (using_ramfb) kputln("[com.butterscotch.uiservice]: RAMFB fallback active (no vblank synchronization)");
	kputln("[com.butterscotch.uiservice]: starting About sevOS");

	/*
	 * Stop the emergency framebuffer console from writing underneath UI while
	 * the graphical session owns RAMFB. UART logging remains fully active.
	 */
	if (using_ramfb) {
		if (!ramfb_console_set_mirroring(false)) {
			kputln("[com.butterscotch.uiservice]: failed to suspend RAMFB console mirroring");
			(void)kfree(backbuffer);
			return false;
		}
	}

	uint32_t status = UIServiceRunAbout(&host);
	if (using_ramfb) (void)ramfb_console_set_mirroring(true);

	kprintf("[com.butterscotch.uiservice]: About.app exited unexpectedly with status %u\n", status);
	(void)kfree(backbuffer);
	return false;
#endif
}
