/* An ordinary app verifies the copied desktop bridge and input routing. */
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/gui_host_protocol.h>
#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>

static uint32_t pixels[160 * 100];

static void
log_message(const char *text)
{
	(void)nxu_write(1, text, nxu_strlen(text));
}

#include "event_wait_probe.h"

int
main(void)
{
	if (!event_wait_probe()) return 90;
	nxu_gui_host_hello_t hello = {0};
	hello.header = (nxu_gui_host_header_t){sizeof(hello), 1, 1, NXU_GUI_HOST_HELLO, 0, 0, 1};
	hello.required_features = hello.supported_features = NXU_GUI_HOST_FEATURE_SESSION;
	hello.max_record_size = NXU_GUI_HOST_MAX_RECORD;

	if (nxu_gui_host(&hello, sizeof(hello)) != -NXU_SYS_E_DENIED) {
		log_message("coregraphics-client: FAILED capability isolation\n");
		return 1;
	}

	nxu_ui_session_t session;

	if (nxu_ui_control(NXU_UI_CONTROL_SESSION, (uint64_t)(uintptr_t)&session) < 0) return 2;

	nxu_ui_connect_t info = {0};
	info.struct_size = sizeof(info);
	info.kind = NXU_UI_KIND_APP;
	memcpy(info.name, "CoreGraphics IPC Probe", sizeof("CoreGraphics IPC Probe"));
	info.width = 200;
	info.height = 140;
	info.min_width = 100;
	info.min_height = 80;
	info.max_width = 640;
	info.max_height = 480;
	info.resizable = 1;
	info.titlebar_height = 24;
	info.corner_radius = 8;

	int64_t connection = nxu_ui_connect(&info);

	if (connection <= 0) return 3;

	for (uint32_t i = 0; i < 160 * 100; i++)
		pixels[i] = 0xff00ff00;

	uint32_t progress = 0, stage = 0;

	if (!bootstrap_client_lookup("com.nxu.gui-progress", &progress)) return 5;

	bool painted = false;

	for (;;) {
		nxu_ui_message_t message;
		int64_t received = nxu_ui_receive((uint32_t)connection, &message, 100);

		if (received < 0) return 0;

		if (received == 0) continue;

		if (message.type == NXU_UI_MSG_EVENT) {
			if (message.event == NXU_UI_EVENT_KEY_DOWN && message.character == 'a') {
				log_message("coregraphics-client: KEY\n");
				stage++;
				(void)nxu_ipc_send(progress, &stage, sizeof(stage), 0);
			}

			if (message.event == NXU_UI_EVENT_SCROLL)
				log_message(message.delta > 0 ? "coregraphics-client: SCROLL UP\n"
											  : "coregraphics-client: SCROLL DOWN\n");
			if (message.event == NXU_UI_EVENT_POINTER_DOWN) log_message("coregraphics-client: CLICK\n");
		}

		if (message.type == NXU_UI_MSG_REDRAW || !painted) {
			nxu_ui_submit_t submit = {0};
			submit.pixels = (uint64_t)(uintptr_t)pixels;
			submit.width = 160;
			submit.height = 100;
			submit.stride = 160;

			if (nxu_ui_submit((uint32_t)connection, &submit) < 0) return 4;

			if (!painted) log_message("coregraphics-client: FRAME\n");

			painted = true;
		}
	}
}
