#include <CoreGraphics.h>
#include <DesktopService.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

int main(void)
{
	CoreGraphicsDirEntry entries[128];
	uint32_t count = 0;
	bool truncated = false;
	const char *path = "/disk/System/Library/CoreServices";

	if (DesktopServiceListDirectory(0, path, nxu_strlen(path), entries, 128, &count, &truncated) !=
			CORE_GRAPHICS_FS_STATUS_OK ||
		!count || truncated)
		return 1;

	bool found = false;

	for (uint32_t i = 0; i < count; i++)
		if (nxu_streq(entries[i].name, "bootd") && entries[i].size_bytes &&
			entries[i].kind == CORE_GRAPHICS_FS_KIND_REGULAR)
			found = true;

	if (!found ||
		DesktopServiceListDirectory(0, path, nxu_strlen(path), 0, 0, &count, &truncated) !=
			CORE_GRAPHICS_FS_STATUS_OK ||
		count || !truncated)
		return 2;

	path = "/disk/System/Library/CoreServices/bootd";

	if (DesktopServiceListDirectory(0, path, nxu_strlen(path), entries, 128, &count, &truncated) !=
		CORE_GRAPHICS_FS_STATUS_NOT_A_DIRECTORY)
		return 3;

	path = "/disk/does-not-exist";

	if (DesktopServiceListDirectory(0, path, nxu_strlen(path), entries, 128, &count, &truncated) !=
		CORE_GRAPHICS_FS_STATUS_NOT_FOUND)
		return 4;

	CoreGraphicsActivity activity = {.struct_size = sizeof(activity)};
	CoreGraphicsProcessInfo processes[128];

	if (DesktopServiceGetActivity(0, &activity, processes, 128, &count) != CORE_GRAPHICS_STATUS_OK ||
		!activity.cpu_count || !activity.total_pages || !count)
		return 5;

	found = false;

	for (uint32_t i = 0; i < count; i++)
		if (processes[i].pid == (uint32_t)nxu_getpid() && processes[i].uniqueid)
			found = true;

	if (!found)
		return 6;

	activity.struct_size = 0;

	if (DesktopServiceGetActivity(0, &activity, processes, 128, &count) != CORE_GRAPHICS_STATUS_INVALID_ARGUMENT)
		return 7;

	static const char passed[] = "host-callbacks: PASS directory errors/truncation/metadata and activity snapshots\n";
	(void)nxu_write(1, passed, sizeof(passed) - 1);
	/* Keep this retryable test job alive after its one successful check. */
	for (;;)
		nxu_sleep_us(1000000);
}
