/*
 * launchtest -- launch apps one after another while the desktop is up, to
 * see what an app launch costs the user interface (the desktop logs every
 * 16.7 ms frame it misses; see ui-service-nxu's budget.rs).
 *
 *   launchtest [PATH...]
 *
 * Waits 12 s for the desktop to settle, then spawns each PATH (by default
 * the bundled apps) 8 s apart, logging when it asked and how long the spawn
 * call took.
 */
#include <nxu/string.h>
#include <nxu/syscall.h>

static const char *const default_apps[] = {
	"/Volumes/mainOS/Applications/Voyager.app/Contents/GroveOS/Voyager",
	"/Volumes/mainOS/Applications/System Settings.app/Contents/GroveOS/System Settings",
	"/Volumes/mainOS/System/Library/CoreServices/About groveOS.app/Contents/GroveOS/About groveOS",
	"/Volumes/mainOS/Applications/Activity Monitor.app/Contents/GroveOS/Activity Monitor",
};

static void
say(const char *const *parts)
{
	char line[256];
	uint32_t length = 0;

	for (; *parts; parts++)
		for (const char *text = *parts; *text && length + 2 < sizeof(line); text++) line[length++] = *text;
	line[length++] = '\n';
	(void)nxu_write(1, line, length);
}

static const char *
decimal(uint64_t value, char digits[24])
{
	char *cursor = digits + 23;

	*cursor = 0;
	do {
		*--cursor = (char)('0' + value % 10U);
		value /= 10U;
	} while (value);
	return cursor;
}

static const char *
leaf(const char *path)
{
	const char *name = path;

	for (; *path; path++)
		if (*path == '/') name = path + 1;
	return name;
}

int
main(uint64_t argc, const char *const *argv)
{
	const char *const *apps = argc > 1 ? argv + 1 : default_apps;
	uint64_t count = argc > 1 ? argc - 1 : sizeof(default_apps) / sizeof(default_apps[0]);
	char digits[24];

	(void)nxu_sleep_us(12000000);
	for (uint64_t index = 0; index < count; index++) {
		int64_t asked = nxu_uptime_us();
		int64_t pid = nxu_spawn(apps[index], leaf(apps[index]));
		int64_t took = nxu_uptime_us() - asked;

		say((const char *[]){"launchtest: ", leaf(apps[index]), pid < 0 ? ": failed" : ": spawned, the call took ",
							 pid < 0 ? "" : decimal((uint64_t)took, digits), pid < 0 ? "" : " us", 0});
		(void)nxu_sleep_us(8000000);
	}
	say((const char *[]){"launchtest: done", 0});
	return 0;
}
