/* Observe a production boot and request app launches through the real Dock. */
#include <nxu/activity.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

static nxu_ui_activity_t activity;
static nxu_ui_process_info_t processes[128];

static bool
snapshot(uint32_t *count)
{
	activity.struct_size = sizeof(activity);
	nxu_ui_activity_request_t request = {
		.activity = (uint64_t)(uintptr_t)&activity,
		.processes = (uint64_t)(uintptr_t)processes,
		.activity_size = sizeof(activity),
		.process_size = sizeof(processes[0]),
		.capacity = 128,
	};
	if (nxu_ui_control(NXU_UI_CONTROL_ACTIVITY, (uint64_t)(uintptr_t)&request) < 0) return false;
	*count = request.count;
	return activity.cpu_count && activity.total_pages;
}

static bool
contains(uint32_t count, const char *name)
{
	for (uint32_t i = 0; i < count; i++)
		if (nxu_streq(processes[i].name, name)) return true;
	return false;
}

int
main(void)
{
	nxu_ui_session_t session;
	if (nxu_ui_control(NXU_UI_CONTROL_SESSION, (uint64_t)(uintptr_t)&session) < 0) return 1;

	bool saw_sap = false;
	uint64_t deadline = (uint64_t)nxu_uptime_us() + 120000000;
	uint32_t count = 0;
	while ((uint64_t)nxu_uptime_us() < deadline) {
		if (!snapshot(&count)) return 2;
		bool active = contains(count, "saptest");
		saw_sap |= active;
		if (saw_sap && !active) break;
		(void)nxu_sleep_us(100000);
	}
	if (!saw_sap || contains(count, "saptest")) return 3;

	static const char *bundles[] = {
		"/Applications/Voyager.app",
		"/Applications/Activity Monitor.app",
		"/System/Library/CoreServices/About sevOS.app",
	};
	for (uint32_t i = 0; i < 3; i++)
		if (nxu_ui_control(NXU_UI_CONTROL_LAUNCH, (uint64_t)(uintptr_t)bundles[i]) < 0) return 4;

	deadline = (uint64_t)nxu_uptime_us() + 60000000;
	while ((uint64_t)nxu_uptime_us() < deadline) {
		if (!snapshot(&count)) return 5;
		if (contains(count, "Dock") && contains(count, "Voyager") && contains(count, "Activity Monitor") &&
			contains(count, "About sevOS")) {
			static const char passed[] =
				"production-session: PASS Dock launches, app processes and activity snapshots\n";
			(void)nxu_write(1, passed, sizeof(passed) - 1);
			if (nxu_spawn("/disk/System/Library/CoreServices/crashtest", "production-crash-probe") <= 0) return 6;
			return 0;
		}
		(void)nxu_sleep_us(100000);
	}
	return 7;
}
