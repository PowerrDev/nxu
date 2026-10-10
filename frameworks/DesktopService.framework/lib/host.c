/* Desktop host callbacks use bounded public syscalls, not kernel imports. */
#include <CoreGraphics.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

uint32_t DesktopServiceScale(void)
{
	char arguments[1024] = {0};

	if (nxu_get_boot_args(arguments, sizeof(arguments)) < 0)
		return 1000;

	for (uint32_t cursor = 0; arguments[cursor];) {
		while (arguments[cursor] == ' ')
			cursor++;

		uint32_t start = cursor;

		while (arguments[cursor] && arguments[cursor] != ' ')
			cursor++;

		uint32_t length = cursor - start;

		if (length <= 9)
			continue;

		bool matched = true;
		const char prefix[] = "ui.scale=";

		for (uint32_t i = 0; i < 9; i++)
			matched &= arguments[start + i] == prefix[i];

		if (!matched)
			continue;

		uint32_t scale = 0;

		for (uint32_t i = start + 9; i < cursor; i++) {
			if (arguments[i] < '0' || arguments[i] > '9' || scale > 4000)
				return 1000;

			scale = scale * 10 + arguments[i] - '0';
		}

		return scale >= 500 && scale <= 4000 ? scale : 1000;
	}

	return 1000;
}

uint32_t DesktopServiceGetActivity(void *context, CoreGraphicsActivity *activity, CoreGraphicsProcessInfo *processes,
								   uint32_t capacity, uint32_t *count)
{
	(void)context;

	if (!activity || !count || (capacity && !processes) || activity->struct_size != sizeof(*activity))
		return CORE_GRAPHICS_STATUS_INVALID_ARGUMENT;

	*count = 0;
	nxu_ui_activity_request_t request = {
		.activity = (uint64_t)(uintptr_t)activity,
		.processes = (uint64_t)(uintptr_t)processes,
		.activity_size = sizeof(*activity),
		.process_size = sizeof(*processes),
		.capacity = capacity,
	};

	if (nxu_ui_control(NXU_UI_CONTROL_ACTIVITY, (uint64_t)(uintptr_t)&request) < 0)
		return CORE_GRAPHICS_STATUS_UNSUPPORTED;

	*count = request.count;
	return CORE_GRAPHICS_STATUS_OK;
}

uint32_t DesktopServiceListDirectory(void *context, const char *path, uint32_t length, CoreGraphicsDirEntry *entries,
									 uint32_t capacity, uint32_t *count, bool *truncated)
{
	(void)context;

	if (!path || !count || !truncated || (capacity && !entries) || !length || length >= 512)
		return CORE_GRAPHICS_FS_STATUS_ERROR;

	*count = 0;
	*truncated = false;
	char directory[512];
	memcpy(directory, path, length);
	directory[length] = 0;

	for (uint32_t i = 0; i < length; i++)
		if (!directory[i])
			return CORE_GRAPHICS_FS_STATUS_ERROR;

	nxu_stat_t stat;

	int64_t found = nxu_stat(directory, &stat);

	if (found < 0)
		return found == -NXU_SYS_E_NOT_FOUND ? CORE_GRAPHICS_FS_STATUS_NOT_FOUND : CORE_GRAPHICS_FS_STATUS_ERROR;

	if (stat.type != NXU_STAT_TYPE_DIRECTORY)
		return CORE_GRAPHICS_FS_STATUS_NOT_A_DIRECTORY;

	int64_t descriptor = nxu_open(directory, 1);

	if (descriptor < 0)
		return CORE_GRAPHICS_FS_STATUS_ERROR;

	uint32_t status = CORE_GRAPHICS_FS_STATUS_OK;
	nxu_dirent_t item;
	int64_t read;

	while ((read = nxu_readdir((uint64_t)descriptor, &item)) > 0) {
		if (nxu_streq(item.name, ".") || nxu_streq(item.name, ".."))
			continue;

		if (*count == capacity) {
			*truncated = true;
			break;
		}

		CoreGraphicsDirEntry *entry = &entries[(*count)++];
		*entry = (CoreGraphicsDirEntry){0};
		entry->name_length = item.name_length;
		uint32_t copied = item.name_length < CORE_GRAPHICS_FS_NAME_MAX ? item.name_length : CORE_GRAPHICS_FS_NAME_MAX;
		memcpy(entry->name, item.name, copied);
		if (item.type == NXU_DIRENT_TYPE_DIRECTORY)
			entry->kind = CORE_GRAPHICS_FS_KIND_DIRECTORY;
		else if (item.type == NXU_DIRENT_TYPE_REGULAR)
			entry->kind = CORE_GRAPHICS_FS_KIND_REGULAR;
		else
			entry->kind = CORE_GRAPHICS_FS_KIND_OTHER;

		if (entry->kind == CORE_GRAPHICS_FS_KIND_REGULAR && length + item.name_length + 2 <= sizeof(directory)) {
			directory[length] = '/';
			memcpy(directory + length + 1, item.name, item.name_length + 1);

			if (nxu_stat(directory, &stat) == 0)
				entry->size_bytes = stat.size;

			directory[length] = 0;
		}
	}

	int64_t closed = nxu_close((uint64_t)descriptor);

	if (read < 0 || closed < 0)
		status = CORE_GRAPHICS_FS_STATUS_ERROR;

	return status;
}
