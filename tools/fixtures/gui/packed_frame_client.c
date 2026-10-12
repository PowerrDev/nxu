/* Architecture-neutral app checks packed/padded frames and copy-fault handling. */
#include <nxu/string.h>
#include <nxu/syscall.h>

static uint32_t packed[160 * 100];
static uint32_t padded[192 * 100];

static void log_message(const char *text)
{
	(void)nxu_write(1, text, nxu_strlen(text));
}


#include "event_wait_probe.h"

int main(void)
{
	if (!event_wait_probe()) return 90;
	nxu_ui_session_t session;
	uint64_t deadline = nxu_uptime_us() + 120000000;
	while (nxu_ui_control(NXU_UI_CONTROL_SESSION, (uint64_t)(uintptr_t)&session) < 0) {
		if ((uint64_t)nxu_uptime_us() >= deadline)
			return 1;
		(void)nxu_sleep_us(10000);
	}

	nxu_ui_connect_t info = {.struct_size = sizeof(info),
							 .kind = NXU_UI_KIND_APP,
							 .width = 200,
							 .height = 140,
							 .min_width = 200,
							 .min_height = 140,
							 .max_width = 200,
							 .max_height = 140,
							 .titlebar_height = 24,
							 .corner_radius = 8};
	memcpy(info.name, "Frame Copy Probe", sizeof("Frame Copy Probe"));
	int64_t connection = nxu_ui_connect(&info);
	if (connection <= 0)
		return 2;

	for (uint32_t i = 0; i < 160 * 100; i++)
		packed[i] = 0xff00ff00;
	for (uint32_t y = 0; y < 100; y++)
		for (uint32_t x = 0; x < 192; x++)
			padded[y * 192 + x] = x < 160 ? 0xff0000ff : 0xffff0000;

	nxu_ui_submit_t submit = {.pixels = (uint64_t)(uintptr_t)packed, .width = 160, .height = 100, .stride = 160};
	if (nxu_ui_submit((uint32_t)connection, &submit) < 0)
		return 3;
	log_message("frame-copy-probe: PACKED\n");

	for (;;) {
		nxu_ui_message_t message;
		if (nxu_ui_receive((uint32_t)connection, &message, NXU_UI_WAIT_FOREVER) < 0)
			return 4;
		if (message.type != NXU_UI_MSG_EVENT)
			continue;
		if (message.event == NXU_UI_EVENT_SCROLL)
			log_message(message.delta > 0 ? "frame-copy-probe: SCROLL UP\n" : "frame-copy-probe: SCROLL DOWN\n");
		if (message.event == NXU_UI_EVENT_POINTER_DOWN)
			log_message("frame-copy-probe: CLICK\n");
		if (message.event == NXU_UI_EVENT_KEY_DOWN && message.character == 'a') {
			submit.pixels = (uint64_t)(uintptr_t)padded;
			submit.stride = 192;
			if (nxu_ui_submit((uint32_t)connection, &submit) < 0)
				return 5;
			submit.pixels = 0x1000;
			if (nxu_ui_submit((uint32_t)connection, &submit) != -NXU_SYS_E_BAD_ADDRESS)
				return 6;
			log_message("frame-copy-probe: PADDED and FAULT\n");
		}
		if (message.event == NXU_UI_EVENT_KEY_DOWN && message.character == 'b') {
			log_message("frame-copy-probe: EXIT\n");
			return 0;
		}
	}
}
