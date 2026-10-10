#include <nxu/syscall.h>
#include <nxu/string.h>
#include <stdint.h>
static uint32_t ready, active, leaked, disconnected, presented;
static uint32_t pixels[200 * 150];
static const char *keep = "/disk/System/Library/Frameworks/SAPTest.framework/keep.txt";
static const char *remove_path = "/disk/System/Library/Frameworks/SAPTest.framework/remove.txt";

static void output(const char *text)
{
	(void)nxu_write(1, text, nxu_strlen(text));
}

static void probe(void *unused)
{
	(void)unused;
	nxu_ui_connect_t info = {.struct_size = sizeof(info),
							 .kind = NXU_UI_KIND_APP,
							 .width = 200,
							 .height = 150,
							 .min_width = 200,
							 .min_height = 150,
							 .max_width = 200,
							 .max_height = 150,
							 .titlebar_height = 32,
							 .corner_radius = 8};
	memcpy(info.name, "SAP input probe", 16);
	int64_t connection = nxu_ui_connect(&info);
	if (connection < 0) {
		__atomic_store_n(&ready, 2, __ATOMIC_RELEASE);
		return;
	}
	for (uint32_t index = 0; index < 200 * 150; index++)
		pixels[index] = 0xff00ff00;

	nxu_ui_submit_t submit = {.pixels = (uint64_t)(uintptr_t)pixels,
							  .width = 200,
							  .height = 150,
							  .stride = 200,
							  .flags = NXU_UI_SUBMIT_ANIMATING};

	if (nxu_ui_submit((uint32_t)connection, &submit) < 0) {
		__atomic_store_n(&ready, 2, __ATOMIC_RELEASE);
		return;
	}

	__atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
	for (;;) {
		nxu_ui_message_t message;
		if (nxu_ui_receive(connection, &message, NXU_UI_WAIT_FOREVER) < 0) {
			__atomic_store_n(&disconnected, 1, __ATOMIC_RELEASE);
			return;
		}

		if (message.type == NXU_UI_MSG_FRAME)
			__atomic_store_n(&presented, 1, __ATOMIC_RELEASE);
		if (message.type == NXU_UI_MSG_EVENT && message.event == NXU_UI_EVENT_KEY_DOWN && message.character &&
			__atomic_load_n(&active, __ATOMIC_ACQUIRE))
			__atomic_add_fetch(&leaked, 1, __ATOMIC_RELAXED);
	}
}

int main(void)
{
	nxu_ui_session_t session;
	if (nxu_ui_control(NXU_UI_CONTROL_SESSION, (uint64_t)&session) < 0)
		return 1;
	int64_t stack = nxu_mmap(16384, 0);
	if (stack < 0 || nxu_thread_create(probe, (void *)(uintptr_t)(stack + 16384), 0) < 0)
		return 2;
	while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) == 0)
		nxu_sleep_us(10000);
	if (ready != 1)
		return 3;
	for (uint32_t poll = 0; poll < 500 && !__atomic_load_n(&presented, __ATOMIC_ACQUIRE); poll++)
		nxu_sleep_us(10000);

	if (!__atomic_load_n(&presented, __ATOMIC_ACQUIRE))
		return 10;

	__atomic_store_n(&active, 1, __ATOMIC_RELEASE);
	output("saptest: cancel phase\n");
	if (nxu_sap_delete(keep) != -NXU_SYS_E_DENIED)
		return 4;
	nxu_stat_t stat;
	if (nxu_stat(keep, &stat) < 0)
		return 5;
	if (nxu_ui_control(NXU_UI_CONTROL_SESSION, (uint64_t)&session) < 0)
		return 12;

	/* After a desktop restart the same app connection is adopted again. */
	__atomic_store_n(&presented, 0, __ATOMIC_RELEASE);
	for (uint32_t poll = 0; poll < 1000 && !__atomic_load_n(&presented, __ATOMIC_ACQUIRE); poll++)
		nxu_sleep_us(10000);

	if (!__atomic_load_n(&presented, __ATOMIC_ACQUIRE))
		return 13;

	output("saptest: rejected-password phase\n");
	if (nxu_unlink(keep) != -NXU_SYS_E_DENIED || nxu_stat(keep, &stat) < 0)
		return 6;
	/* Compositor recovery may finish after the rejected-password modal. */
	if (nxu_ui_control(NXU_UI_CONTROL_SESSION, (uint64_t)&session) < 0)
		return 14;

	output("saptest: authorized-delete phase\n");
	if (nxu_unlink(remove_path) != 0 || nxu_stat(remove_path, &stat) != -NXU_SYS_E_NOT_FOUND)
		return 7;
	if (nxu_sap_delete("/disk/System/Library/Frameworks") >= 0 ||
		nxu_sap_delete("/disk/System/../System/Library/Frameworks/SAPTest.framework/keep.txt") >= 0)
		return 8;
	__atomic_store_n(&active, 0, __ATOMIC_RELEASE);
	nxu_sleep_us(50000);
	if (__atomic_load_n(&leaked, __ATOMIC_ACQUIRE) != 0) {
		output("saptest: FAILED password reached app input\n");
		return 9;
	}
	if (__atomic_load_n(&disconnected, __ATOMIC_ACQUIRE)) {
		output("saptest: FAILED app session was lost during trusted UI\n");
		return 11;
	}

	output("saptest: restored app surface\n");
	nxu_sleep_us(3000000);
	output("saptest: PASS cancellation, wrong password, exact-file deletion, no recursive deletion, exclusive input\n");
	return 0;
}
