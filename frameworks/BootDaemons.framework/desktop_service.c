/* Userland desktop host. DesktopService owns UI policy; WindowServer owns scanout. */
#include <CoreGraphics.h>
#include <drivers/video/ui_service_pointer.h>
#include <DesktopService.h>
#include <nxu/runtime.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

static struct {
	CoreGraphicsSurfaceDescriptor surface;
	int32_t x, y, raw_x, raw_y;
	uint32_t buttons, down, up;
	bool baseline, moved, idle_once;
	uint32_t scale;
	ui_pointer_motion_t motion;
} desktop;

static uint32_t get_surface(void *context, CoreGraphicsSurfaceDescriptor *surface)
{
	(void)context;

	if (!surface)
		return CORE_GRAPHICS_STATUS_INVALID_ARGUMENT;

	*surface = desktop.surface;
	return CORE_GRAPHICS_STATUS_OK;
}

static uint32_t present(void *context, const CoreGraphicsDamageRect *damage)
{
	(void)context;
	(void)damage;
	/* Render_Window sends the completed surfaces to the compositor over NXPC. */
	return CORE_GRAPHICS_STATUS_OK;
}

static int32_t clamp(int64_t value, uint32_t extent)
{
	if (value < 0)
		return 0;

	return (uint64_t)value >= extent ? (int32_t)(extent - 1) : (int32_t)value;
}

static uint32_t key_character(uint32_t code, uint32_t modifiers)
{
	static const char normal[] = "\0\0"
								 "1234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
	static const char shifted[] = "\0\0"
								  "!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

	if (code >= sizeof(normal) - 1)
		return 0;

	char character = (modifiers & 1) ? shifted[code] : normal[code];

	if ((modifiers & 8) && character >= 'a' && character <= 'z')
		character -= 'a' - 'A';
	else if ((modifiers & 8) && character >= 'A' && character <= 'Z')
		character += 'a' - 'A';

	/* Terminal control chords use the conventional ASCII control characters. */
	if ((modifiers & 2) && ((character >= 'a' && character <= 'z') ||
		(character >= 'A' && character <= 'Z'))) {
		character &= 0x1f;
	}

	return (uint8_t)character;
}

static uint32_t poll_event(void *context, CoreGraphicsHostEvent *event)
{
	(void)context;

	if (!CoreGraphicsConnectionAlive()) {
		static const char restart[] = "desktop: compositor lost; restarting session\n";
		(void)nxu_write(1, restart, sizeof(restart) - 1);
		/* Exit directly: Rust destructors would release the app connections
		 * that the replacement desktop must adopt. */
		nxu_exit(0);
	}

	if (!event)
		return CORE_GRAPHICS_STATUS_INVALID_ARGUMENT;

	*event = (CoreGraphicsHostEvent){.struct_size = sizeof(*event)};

	uint32_t sequence = (uint32_t)nxu_event_wait(0, 0);
	for (;;) {
		event->x = desktop.x;
		event->y = desktop.y;

		if (desktop.moved) {
			desktop.moved = false;
			event->event_type = CORE_GRAPHICS_EVENT_POINTER_MOVED;
			return CORE_GRAPHICS_STATUS_OK;
		}

		for (uint32_t button = 0; button < 3; button++) {
			uint32_t mask = 1U << button;

			if (desktop.down & mask) {
				desktop.down &= ~mask;
				event->event_type = CORE_GRAPHICS_EVENT_POINTER_DOWN;
				event->button = button + 1;
				return CORE_GRAPHICS_STATUS_OK;
			}

			if (desktop.up & mask) {
				desktop.up &= ~mask;
				event->event_type = CORE_GRAPHICS_EVENT_POINTER_UP;
				event->button = button + 1;
				return CORE_GRAPHICS_STATUS_OK;
			}
		}

		nxu_recovery_input_event_t input;
		int64_t received = nxu_recovery_input(&input);

		if (received < 0) {
			desktop.baseline = false;
			desktop.down = desktop.up = desktop.buttons = 0;
			return CORE_GRAPHICS_STATUS_OK;
		}

		if (!received) {
			if (desktop.idle_once)
				(void)nxu_event_wait(16667, sequence);
			desktop.idle_once = true;
			return CORE_GRAPHICS_STATUS_OK;
		}
		desktop.idle_once = false;

		if (input.kind == NXU_RECOVERY_INPUT_SCROLL) {
			event->event_type = CORE_GRAPHICS_EVENT_SCROLL;
			event->reserved = (uint32_t)input.value;
			return CORE_GRAPHICS_STATUS_OK;
		}

		if (input.kind == NXU_RECOVERY_INPUT_KEY) {
			if (input.value != 1 && input.value != 2)
				continue;

			event->event_type = CORE_GRAPHICS_EVENT_KEY_DOWN;
			event->button = input.code;
			event->reserved = key_character(input.code, input.modifiers);
			return CORE_GRAPHICS_STATUS_OK;
		}

		if (input.kind != NXU_RECOVERY_INPUT_POINTER)
			continue;

		if (!desktop.baseline) {
			desktop.raw_x = input.x;
			desktop.raw_y = input.y;
			desktop.baseline = true;
		}

		int32_t dx = (int64_t)input.x - desktop.raw_x;
		int32_t dy = (int64_t)input.y - desktop.raw_y;
		ui_pointer_scale(&desktop.motion, desktop.scale, &dx, &dy);
		int32_t x = clamp((int64_t)desktop.x + dx, desktop.surface.width);
		int32_t y = clamp((int64_t)desktop.y + dy, desktop.surface.height);
		desktop.raw_x = input.x;
		desktop.raw_y = input.y;
		desktop.moved = x != desktop.x || y != desktop.y;
		desktop.x = x;
		desktop.y = y;

		if (desktop.moved)
			(void)CG_Pointer_Move(x, y);
		desktop.down = input.buttons & ~desktop.buttons;
		desktop.up = desktop.buttons & ~input.buttons;
		desktop.buttons = input.buttons;
	}
}

static uint32_t get_time(void *context, uint64_t *seconds)
{
	(void)context;
	return DesktopServiceGetTime(seconds) ? CORE_GRAPHICS_STATUS_OK : CORE_GRAPHICS_STATUS_UNSUPPORTED;
}

int main(void)
{
	uint32_t width, height;

	while (!CoreGraphicsDisplaySize(&width, &height))
		(void)nxu_sleep_us(100000);

	if (width > 3840 || height > 2160)
		return 1;

	uint32_t *pixels = nxu_alloc((uint64_t)width * height * sizeof(uint32_t), 16);

	if (!pixels)
		return 2;

	desktop.surface =
		(CoreGraphicsSurfaceDescriptor){pixels, width, height, width, CORE_GRAPHICS_PIXEL_FORMAT_XRGB8888};
	desktop.scale = DesktopServiceScale();
	desktop.x = width / 2;
	desktop.y = height / 2;

	while (!DesktopServiceBeginSession(desktop.scale, width, height, 24 * desktop.scale / 1000))
		(void)nxu_sleep_us(100000);

	CoreGraphicsHostV5 host = {
		.header = {sizeof(host), CORE_GRAPHICS_ABI_VERSION_V5},
		.capabilities = CORE_GRAPHICS_HOST_CAPABILITIES_V3 | CORE_GRAPHICS_HOST_CAP_KEYBOARD |
						CORE_GRAPHICS_HOST_CAP_FS | CORE_GRAPHICS_HOST_CAP_ACTIVITY,
		.get_surface = get_surface,
		.present = present,
		.poll_event = poll_event,
		.get_time = get_time,
		.content_scale_permille = desktop.scale,
		.list_directory = DesktopServiceListDirectory,
		.get_activity = DesktopServiceGetActivity,
	};

	static const char ready[] = "desktop: dynamic host ready\n";
	(void)nxu_write(1, ready, sizeof(ready) - 1);

	uint32_t status = DesktopServiceRun(&host);
	DesktopServiceShutdown();
	(void)nxu_free(pixels);
	return status ? 4 : 0;
}
