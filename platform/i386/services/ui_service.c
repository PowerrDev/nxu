#include <drivers/video/ui_service_host.h>
#include <drivers/video/ui_service_pointer.h>

#include <kern/machine/timer.h>
#include <drivers/input/mouse.h>
#include <drivers/video/display.h>
#include <skylight/skylight.h>
#include <drivers/virtio/virtio_input.h>
#include <kern/i386/boot_info.h>
#include <kern/console/console.h>
#include <kern/memory/heap.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <platform/rtc.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(NXU_UI_SERVICE)
#include <UIService.h>
#include <drivers/video/ui_service_activity.h>
#include <drivers/video/ui_service_bridge.h>
#include <drivers/video/ui_service_login.h>
#if defined(NXU_WINDOWSERVER)
#include <WindowServer/WSPrivate.h>
#endif
#endif

#define UI_SERVICE_FRAME_INTERVAL_US 16667ULL

/*
 * Presents closer together than this wait. Not a whole frame interval: a
 * sleep ends on a 10 ms timer tick, so waiting out 16.7 ms took 20 ms (50 Hz,
 * and a frame late). With the GPU's own completion now slept for, this only
 * keeps a burst of partial updates from flooding the host.
 */
#define UI_SERVICE_MIN_PRESENT_US 8000ULL

static bool g_ui_service_cooperative;
static volatile bool g_ui_service_running;
static volatile uint64_t g_ui_service_polls;

void ui_service_set_cooperative(bool cooperative)
{
	g_ui_service_cooperative = cooperative;
}

bool ui_service_running(void)
{
	return g_ui_service_running;
}

uint64_t ui_service_poll_count(void)
{
	return g_ui_service_polls;
}

/*
 * A decimal boot argument, i386-style: this port has no NVRAM-backed
 * kern/boot/boot_args.c (Multiboot's command line is read straight off
 * i386_boot_arg(), the same mechanism platform/i386/devices_init.c uses for
 * every other boot argument on this port). Kept local and tiny rather than
 * pulling the NVRAM/boot_args subsystem in for one value.
 */
#if defined(NXU_UI_SERVICE)
static uint32_t ui_service_arg_uint32(const char *name, uint32_t default_value)
{
	char text[16];

	if (!i386_boot_arg(name, text, sizeof(text)) || text[0] == '\0') return default_value;

	uint32_t result = 0U;

	for (uint32_t index = 0U; text[index] != '\0'; index++) {
		if (text[index] < '0' || text[index] > '9') return default_value;

		result = result * 10U + (uint32_t)(text[index] - '0');
	}

	return result;
}
#endif

#if defined(NXU_UI_SERVICE)
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t *output_pixels;
	uint32_t *backbuffer;
	/* The Skylight display presented to, or 0 for the RAMFB fallback (written directly). */
	sl_display_t *display;
	uint64_t last_present_us;
	int32_t pointer_x;
	int32_t pointer_y;
	/* Mouse motion is in host points; the pointer moves in pixels (see ui_service_pointer.h). */
	uint32_t scale_permille;
	ui_pointer_motion_t motion;
	uint32_t buttons;
	uint32_t pending_down;
	uint32_t pending_up;
	bool pending_move;
	/* The driver's running wheel total at the last poll, and the notches turned since the app was last told. */
	int64_t last_wheel;
	int32_t pending_scroll;
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
 * Pace software presents near 60 Hz. VirtIO GPU has no real vblank handshake
 * on this port either, so this cannot provide hardware VSYNC, but it avoids
 * flooding the scanout with many partially overlapping updates while a
 * window is being dragged.
 *
 * Cooperative waits do it by sleeping (sched_sleep_us), not by spinning on
 * sched_yield(): a yield only gives up *this* turn and immediately asks for
 * another, so a wait of several milliseconds became dozens of scheduler
 * round trips fighting everything else on the boot CPU for a turn. A thread
 * that blocks instead keeps its MLFQ level (only *using* a full quantum
 * without blocking demotes it -- see sched.c), so pacing this way is also
 * what keeps the UI thread's own scheduling priority high without a manual
 * override that MLFQ's own demotion would just undo on the next quantum
 * anyway. See platform/arm64/services/ui_service.c, ported from verbatim.
 */
static void UIServicePaceFrame(UIServiceContext *context)
{
	uint64_t now = timer_get_microseconds();
	if (now == 0ULL) return;

	if (context->last_present_us != 0ULL) {
		uint64_t elapsed = now - context->last_present_us;
		while (elapsed < UI_SERVICE_MIN_PRESENT_US) {
			if (g_ui_service_cooperative) (void)sched_sleep_us(1000ULL);
			else __asm__ volatile("pause");
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
 * Present a finished UI frame: VirtIO GPU receives its transfer and flush
 * command after the backbuffer copy.
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

	if (context->display == 0) {
		/* RAMFB: the copy is the present (QEMU shows the memory as it is). */
		return UIServiceCopyDamage(context, present_damage) ? UI_SERVICE_STATUS_OK : UI_SERVICE_STATUS_INVALID_ARGUMENT;
	}

	if (present_damage->x < 0 || present_damage->y < 0 || present_damage->width == 0U || present_damage->height == 0U) return UI_SERVICE_STATUS_INVALID_ARGUMENT;

	/* Through Skylight: the damage into the scanout surface, then presented. */
	sl_surface_t *surface = sl_display_surface(context->display);
	const uint32_t *source = context->backbuffer + (uint64_t)present_damage->y * context->stride + (uint32_t)present_damage->x;
	sl_upload(surface, present_damage->x, present_damage->y, source, present_damage->width, present_damage->height, context->stride);
	return sl_present(context->display, (sl_rect_t) { 0 }) ? UI_SERVICE_STATUS_OK : UI_SERVICE_STATUS_PRESENT_FAILED;
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
 * Translate NXU's relative VirtIO mouse state into a stable absolute UI
 * point.
 *
 * Pops at most one queued packet's motion per call, not the net displacement
 * since the last call: UIServicePollEvent (which calls this) is itself
 * drained in a loop by the NXU bridge until nothing is left, so a burst of
 * packets -- several arriving before the bridge gets around to polling, e.g.
 * a shake gesture's rapid direction reversals -- still reaches WindowServer
 * as that same sequence of discrete samples instead of collapsing into one
 * net move. See mouse_take_delta()'s doc comment for the full reasoning.
 */
static void UIServiceRefreshPointer(UIServiceContext *context)
{
	int32_t dx = 0;
	int32_t dy = 0;

	if (mouse_take_delta(&dx, &dy)) {
		ui_pointer_scale(&context->motion, context->scale_permille, &dx, &dy);

		int64_t next_x = (int64_t)context->pointer_x + dx;
		int64_t next_y = (int64_t)context->pointer_y + dy;
		int32_t pointer_x = UIServiceClampPointer(next_x, context->width);
		int32_t pointer_y = UIServiceClampPointer(next_y, context->height);

		if (pointer_x != context->pointer_x || pointer_y != context->pointer_y) {
			context->pending_move = true;
		}

		context->pointer_x = pointer_x;
		context->pointer_y = pointer_y;
	}

	/*
	 * The buttons by their edges, not just their level: a click that starts
	 * and ends between two polls (the desktop was busy for a moment) is
	 * still a down and an up. A release and a new press of a button that is
	 * held again now cancel out.
	 */
	uint32_t pressed = 0U;
	uint32_t released = 0U;
	mouse_take_button_edges(&pressed, &released);
	uint32_t buttons = mouse_buttons();
	uint32_t both = pressed & released;
	uint32_t clicked = both & ~buttons & ~context->buttons;
	uint32_t changed = (buttons ^ context->buttons) | clicked;
	context->pending_down |= (changed & buttons) | clicked;
	context->pending_up |= (changed & context->buttons) | clicked;
	context->buttons = buttons;

	/* The wheel: the driver keeps a running total (positive = wheel up); what the app needs is what turned since it last looked. */
	int64_t wheel = mouse_wheel();
	int64_t turned = wheel - context->last_wheel;

	context->last_wheel = wheel;

	if (turned != 0) {
		int64_t total = (int64_t)context->pending_scroll + turned;

		context->pending_scroll = total > 4096 ? 4096 : total < -4096 ? -4096 : (int32_t)total;
	}

#if defined(NXU_WINDOWSERVER)
	if (context->pending_move) {
		(void)WS_Pointer_Move(context->pointer_x, context->pointer_y);
	}
	if (changed != 0U) {
		uint32_t mask = changed;
		if ((clicked & MOUSE_BUTTON_LEFT) != 0U) {
			(void)WS_Pointer_Button(context->pointer_x, context->pointer_y, 0U, true);
		}
		if ((mask & MOUSE_BUTTON_LEFT) != 0U) {
			(void)WS_Pointer_Button(context->pointer_x, context->pointer_y, 0U, (buttons & MOUSE_BUTTON_LEFT) != 0U);
		}
		if ((mask & MOUSE_BUTTON_RIGHT) != 0U) {
			(void)WS_Pointer_Button(context->pointer_x, context->pointer_y, 1U, (buttons & MOUSE_BUTTON_RIGHT) != 0U);
		}
		if ((mask & MOUSE_BUTTON_MIDDLE) != 0U) {
			(void)WS_Pointer_Button(context->pointer_x, context->pointer_y, 2U, (buttons & MOUSE_BUTTON_MIDDLE) != 0U);
		}
	}
#endif
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
/* At most this many events are handed out back to back before the UI thread lets the rest of the system run. */
#define UI_SERVICE_EVENTS_PER_YIELD 8U

/*
 * The session runs on the boot thread; when other threads and processes share
 * the boot they need their turn, and this is where they get it. A burst is
 * handed out back to back and the others run when the queue is empty, or
 * after UI_SERVICE_EVENTS_PER_YIELD events so a steady stream cannot starve
 * them.
 *
 * An empty poll sleeps instead of yielding, as on arm64 (see
 * platform/arm64/services/ui_service.c). A yield only lets threads at the UI
 * thread's own MLFQ level run, and the processes around the desktop -- bootd,
 * the Dock, the apps, whose file, spawn and console system calls all run on
 * the boot CPU -- have long been demoted below it: with only yields they got
 * no boot-CPU time at all while the desktop was up (the Dock took minutes to
 * start an app). The sleep ends early when an input interrupt or an app
 * kicks it (sched_kick_sleepers).
 */
static void UIServiceYieldAfterPoll(bool delivered_event)
{
	static uint32_t delivered_since_yield;
	/* The poll before this one found nothing either. */
	static bool idle_once;

	if (!g_ui_service_cooperative) return;

	bool sleep = !delivered_event && idle_once;
	idle_once = !delivered_event;

	if (delivered_event && ++delivered_since_yield < UI_SERVICE_EVENTS_PER_YIELD) return;

	delivered_since_yield = 0U;

	/*
	 * A real event (or a burst's last one): give everyone else a single turn
	 * and come straight back. The first empty poll after events also returns
	 * at once, so the desktop presents what those events changed (the cursor,
	 * a window) before anything else; sleeping there made every change wait
	 * out a sleep before it reached the screen. A second empty poll in a row
	 * sleeps until something happens: an input interrupt, an app's frame or
	 * connection (they kick), or the next tick (the clock, animations, the
	 * process checks).
	 */
	if (sleep) (void)sched_sleep_until_kicked(UI_SERVICE_FRAME_INTERVAL_US);
	else (void)sched_yield();
}

static uint32_t UIServicePollEvent(void *opaque, UIServiceHostEvent *event)
{
	if (opaque == 0 || event == 0) return UI_SERVICE_STATUS_INVALID_ARGUMENT;

	UIServiceContext *context = (UIServiceContext *)opaque;

	g_ui_service_polls++;

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
		UIServiceYieldAfterPoll(true);
		return UI_SERVICE_STATUS_OK;
	}

	uint32_t button_mask = UIServiceSelectButtonMask(context->pending_down);
	if (button_mask != 0U) {
		context->pending_down &= ~button_mask;
		event->event_type = UI_SERVICE_EVENT_POINTER_DOWN;
		event->button = UIServiceButtonFromMask(button_mask);
		UIServiceYieldAfterPoll(true);
		return UI_SERVICE_STATUS_OK;
	}

	button_mask = UIServiceSelectButtonMask(context->pending_up);
	if (button_mask != 0U) {
		context->pending_up &= ~button_mask;
		event->event_type = UI_SERVICE_EVENT_POINTER_UP;
		event->button = UIServiceButtonFromMask(button_mask);
		UIServiceYieldAfterPoll(true);
		return UI_SERVICE_STATUS_OK;
	}

	if (context->pending_scroll != 0) {
		/* Everything that turned since the last poll is one event: the app animates towards the total. */
		event->event_type = UI_SERVICE_EVENT_SCROLL;
		event->reserved = (uint32_t)context->pending_scroll;
		context->pending_scroll = 0;
		UIServiceYieldAfterPoll(true);
		return UI_SERVICE_STATUS_OK;
	}

	if (ui_service_poll_key(event)) {
		UIServiceYieldAfterPoll(true);
		return UI_SERVICE_STATUS_OK;
	}

	UIServiceYieldAfterPoll(false);
	return UI_SERVICE_STATUS_OK;
}

/*
 * Report wall-clock time from the CMOS RTC so UIService can draw a live
 * clock in its menu bar without owning any device itself.
 */
static uint32_t UIServiceGetTime(void *opaque, uint64_t *unix_seconds)
{
	(void)opaque;
	if (unix_seconds == 0) return UI_SERVICE_STATUS_INVALID_ARGUMENT;

	*unix_seconds = rtc_unix_time();
	return UI_SERVICE_STATUS_OK;
}

/* The size of the regular file `name` in `directory`, or 0 if it cannot be found or the path is too long. */
static uint64_t UIServiceFileSize(const char *directory, const char *name)
{
	char path[VFS_PATH_MAX];
	size_t directory_length = strlen(directory);
	size_t name_length = strlen(name);

	if (directory_length + 1U + name_length >= sizeof(path)) return 0ULL;

	memcpy(path, directory, directory_length);
	size_t length = directory_length;

	if (length == 0U || path[length - 1U] != '/') path[length++] = '/';

	memcpy(path + length, name, name_length + 1U);

	vnode_t vnode;

	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) return 0ULL;

	uint64_t size = vnode->v_type == VNODE_TYPE_REGULAR ? vnode->v_size : 0ULL;

	vnode_rele(vnode);
	return size;
}

/*
 * List a directory for UIService (its read-only filesystem capability, which
 * Voyager browses with). The listing comes from the VFS the way a system call
 * would read it, into the app's own buffer. The kernel stack this runs on is
 * small, so nothing here is larger than a path or one directory entry.
 */
static uint32_t UIServiceListDirectory(
	void *opaque,
	const char *path,
	uint32_t path_len,
	UIServiceDirEntry *entries,
	uint32_t capacity,
	uint32_t *count_out,
	bool *truncated_out
)
{
	(void)opaque;

	if (path == 0 || entries == 0 || count_out == 0 || truncated_out == 0) return UI_SERVICE_FS_STATUS_ERROR;

	*count_out = 0U;
	*truncated_out = false;

	if (path_len == 0U || path_len >= VFS_PATH_MAX) return UI_SERVICE_FS_STATUS_ERROR;

	char directory[VFS_PATH_MAX];

	memcpy(directory, path, path_len);
	directory[path_len] = '\0';

	vnode_t vnode;
	vfs_status_t status = vfs_lookup(directory, &vnode);

	if (status != VFS_STATUS_OK) return status == VFS_STATUS_NOT_FOUND ? UI_SERVICE_FS_STATUS_NOT_FOUND : UI_SERVICE_FS_STATUS_ERROR;

	bool is_directory = vnode->v_type == VNODE_TYPE_DIRECTORY;

	vnode_rele(vnode);

	if (!is_directory) return UI_SERVICE_FS_STATUS_NOT_A_DIRECTORY;

	filedesc_t filedesc = &proc_kernel()->p_fd;
	uint32_t descriptor;

	if (vfs_open(filedesc, directory, VFS_OPEN_READ, &descriptor) != VFS_STATUS_OK) return UI_SERVICE_FS_STATUS_ERROR;

	uint32_t result = UI_SERVICE_FS_STATUS_OK;
	uint32_t count = 0U;
	vfs_dirent_t dirent;

	for (;;) {
		status = vfs_readdir(filedesc, descriptor, &dirent);

		if (status == VFS_STATUS_END_OF_DIRECTORY) break;

		if (status != VFS_STATUS_OK) {
			result = UI_SERVICE_FS_STATUS_ERROR;
			break;
		}

		/* A browser shows what is inside a folder, not the folder's own links. */
		if (strcmp(dirent.name, ".") == 0 || strcmp(dirent.name, "..") == 0) continue;

		if (count >= capacity) {
			*truncated_out = true;
			break;
		}

		UIServiceDirEntry *entry = &entries[count++];
		size_t length = dirent.name_length < UI_SERVICE_FS_NAME_MAX ? dirent.name_length : UI_SERVICE_FS_NAME_MAX;

		memset(entry, 0, sizeof(*entry));
		memcpy(entry->name, dirent.name, length);
		entry->name_length = dirent.name_length;

		if (dirent.type == VNODE_TYPE_DIRECTORY) {
			entry->kind = UI_SERVICE_FS_KIND_DIRECTORY;
		} else if (dirent.type == VNODE_TYPE_REGULAR) {
			entry->kind = UI_SERVICE_FS_KIND_REGULAR;
			entry->size_bytes = UIServiceFileSize(directory, dirent.name);
		} else {
			entry->kind = UI_SERVICE_FS_KIND_OTHER;
		}
	}

	(void)vfs_close(filedesc, descriptor);
	*count_out = count;
	return result;
}
#endif

bool ui_service_bootstrap(void)
{
#if !defined(NXU_UI_SERVICE)
	kputln("[com.butterscotch.UIService.framework]: NXU bridge is not linked");
	return false;
#else
	/*
	 * No RAMFB emergency console on this port (fw_cfg/ramfb are not built for
	 * i386, see doc/i386/devices.md's known gaps): a VirtIO GPU scanout is
	 * required outright.
	 */
	sl_display_t *display = sl_display_primary();

	if (display == 0) {
		kputln("[com.butterscotch.UIService.framework]: no scanout surface is available");
		return false;
	}

	sl_surface_t *surface = sl_display_surface(display);
	uint32_t stride = 0U;
	uint32_t *framebuffer = sl_surface_map(surface, &stride);
	sl_surface_unmap(surface);
	uint32_t width = sl_surface_width(surface);
	uint32_t height = sl_surface_height(surface);

	if (framebuffer == 0 || width == 0U || height == 0U || stride < width) {
		kputln("[com.butterscotch.UIService.framework]: no scanout surface is available");
		return false;
	}

	if (UIServiceAPIVersion() != UI_SERVICE_API_VERSION) {
		kprintf("[com.butterscotch.UIService.framework]: API mismatch, kernel expects %u but framework reported %u\n", UI_SERVICE_API_VERSION, UIServiceAPIVersion());
		return false;
	}

	size_t pixel_count = (size_t)stride * (size_t)height;
	if (height != 0U && pixel_count / (size_t)height != (size_t)stride) {
		kputln("[com.butterscotch.UIService.framework]: framebuffer size overflow while allocating backbuffer");
		return false;
	}

	uint32_t *backbuffer = kcalloc(pixel_count, sizeof(uint32_t));
	if (backbuffer == 0) {
		kputln("[com.butterscotch.UIService.framework]: software backbuffer allocation failed");
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
		.buttons = mouse_buttons(),
		.pending_down = 0U,
		.pending_up = 0U,
		.pending_move = false,
		.last_wheel = mouse_wheel(),
		.pending_scroll = 0
	};

	/*
	 * Physical pixels per design point, as thousandths. Falls back to the
	 * historical 2x assumption if the boot arg is absent or malformed, same
	 * as arm64.
	 */
	uint32_t content_scale_permille = ui_service_arg_uint32("ui.scale", 2000U);

	context.scale_permille = content_scale_permille;

	UIServiceHostV5 host = {
		.header = {
			.struct_size = sizeof(UIServiceHostV5),
			.abi_version = UI_SERVICE_ABI_VERSION_V5
		},
		.capabilities = UI_SERVICE_HOST_CAPABILITIES_V3 | UI_SERVICE_HOST_CAP_FS | UI_SERVICE_HOST_CAP_ACTIVITY | ui_service_keyboard_capability(),
		.context = &context,
		.get_surface = UIServiceGetSurface,
		.present = UIServicePresent,
		.poll_event = UIServicePollEvent,
		.get_time = UIServiceGetTime,
		.content_scale_permille = content_scale_permille,
		.list_directory = UIServiceListDirectory,
		.get_activity = ui_service_get_activity
	};

	if (UIServiceValidateHostV5(&host) != UI_SERVICE_STATUS_OK) {
		kputln("[com.butterscotch.UIService.framework]: NXU host ABI v5 rejected");
		(void)kfree(backbuffer);
		return false;
	}

	kprintf("[com.butterscotch.UIService.framework]: ABI v%u, %ux%u XRGB8888, VirtIO GPU\n", UIServiceABIVersion(), width, height);
	kputln(UIServiceHasInter() != 0U ? "[com.butterscotch.UIService.framework]: font renderer active" : "[com.butterscotch.UIService.framework]: bootstrap text renderer active");

	/* With a Trusted Enclave link the desktop waits for tepOS to accept the passcode; a refusal keeps it locked. */
	g_ui_service_running = true;
	if (!ui_service_login(&host)) {
		g_ui_service_running = false;
		(void)kfree(backbuffer);
		return false;
	}

	/*
	 * The desktop: no app is linked into the kernel. Apps are processes in
	 * /Applications that bootd's Dock starts; they reach the desktop through
	 * the UI session bridge (drivers/video/ui_service_bridge.c).
	 */
	g_ui_service_running = true;
	uint32_t status = UIServiceRunDesktop(&host);
	g_ui_service_running = false;

	kprintf("[com.butterscotch.UIService.framework]: the desktop exited unexpectedly with status %u\n", status);
	(void)kfree(backbuffer);
	return false;
#endif
}
