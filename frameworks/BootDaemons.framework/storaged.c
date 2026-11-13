/*
 * storaged: what is mounted, how full it is, and what fills it.
 *
 * VOLUME lists the mounted file systems with their size and free space;
 * USAGE walks one volume and adds up its files the way System Settings'
 * Storage pane shows them (Applications, System, Library, Users, Other).
 * A walk is kept for a minute, so asking twice does not walk twice. Reading
 * volumes takes NXU_CAP_STORAGE (AllowStorage in its service plist).
 */
#include <nxu/services.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#define USAGE_CACHE_US 60000000LL
#define WALK_DEPTH_MAX 24U
#define PATH_MAX_LENGTH 256U

static const char *const g_categories[] = {"Applications", "System", "Library", "Users"};
#define CATEGORY_NAMED (sizeof(g_categories) / sizeof(g_categories[0]))
#define CATEGORY_OTHER CATEGORY_NAMED

static struct {
	char mount[NXU_SERVICE_NAME_MAX];
	int64_t taken_us;
	nxu_storage_usage_t usage;
} g_cache;

static void
log_line(const char *first, const char *second)
{
	(void)nxu_write(1, "storaged: ", 10);
	(void)nxu_write(1, first, nxu_strlen(first));
	if (second) (void)nxu_write(1, second, nxu_strlen(second));
	(void)nxu_write(1, "\n", 1);
}

/* `directory` + "/" + `name` into `out`. */
static bool
join(char out[PATH_MAX_LENGTH], const char *directory, const char *name, uint32_t name_length)
{
	uint64_t length = nxu_strlen(directory);
	bool slash = length > 0 && directory[length - 1] != '/';

	if (length + slash + name_length + 1U > PATH_MAX_LENGTH) return false;
	memmove(out, directory, length);
	if (slash) out[length++] = '/';
	memcpy(out + length, name, name_length);
	out[length + name_length] = '\0';
	return true;
}

/* Bytes in the files under `path`, counting them in *files. */
static uint64_t
walk(const char *path, uint32_t depth, uint32_t *files)
{
	if (depth > WALK_DEPTH_MAX) return 0;

	int64_t fd = nxu_open(path, NXU_O_READ);

	if (fd < 0) return 0;

	uint64_t total = 0;
	nxu_dirent_t entry;
	char child[PATH_MAX_LENGTH];

	while (nxu_readdir((uint64_t)fd, &entry) > 0) {
		uint32_t length = entry.name_length > NXU_DIRENT_NAME_MAX ? NXU_DIRENT_NAME_MAX : entry.name_length;

		if ((length == 1 && entry.name[0] == '.') || (length == 2 && entry.name[0] == '.' && entry.name[1] == '.'))
			continue;
		if (!join(child, path, entry.name, length)) continue;

		if (entry.type == NXU_DIRENT_TYPE_DIRECTORY) {
			total += walk(child, depth + 1U, files);
		} else {
			nxu_stat_t info;

			if (nxu_stat(child, &info) == 0) {
				total += info.size;
				(*files)++;
			}
		}
	}
	(void)nxu_close((uint64_t)fd);
	return total;
}

static int64_t
usage(const char *mount, nxu_storage_usage_t *out)
{
	int64_t now = nxu_uptime_us();

	if (g_cache.taken_us && nxu_streq(g_cache.mount, mount) && now - g_cache.taken_us < USAGE_CACHE_US) {
		*out = g_cache.usage;
		return 0;
	}

	nxu_recovery_fs_space_info_t space;
	int64_t status = nxu_recovery_fs_space_info(mount, &space);

	if (status < 0) return status;

	memset(out, 0, sizeof(*out));
	out->total_bytes = space.total_bytes;
	out->free_bytes = space.free_bytes;
	out->category_count = CATEGORY_NAMED + 1U;
	for (uint32_t index = 0; index < CATEGORY_NAMED; index++)
		nxu_service_copy(out->categories[index].name, sizeof(out->categories[index].name), g_categories[index]);
	nxu_service_copy(out->categories[CATEGORY_OTHER].name, sizeof(out->categories[CATEGORY_OTHER].name), "Other");

	int64_t fd = nxu_open(mount, NXU_O_READ);

	if (fd < 0) return fd;

	nxu_dirent_t entry;
	char child[PATH_MAX_LENGTH];

	while (nxu_readdir((uint64_t)fd, &entry) > 0) {
		uint32_t length = entry.name_length > NXU_DIRENT_NAME_MAX ? NXU_DIRENT_NAME_MAX : entry.name_length;

		entry.name[length] = '\0';
		if (nxu_streq(entry.name, ".") || nxu_streq(entry.name, "..")) continue;
		if (!join(child, mount, entry.name, length)) continue;

		uint32_t category = CATEGORY_OTHER;

		for (uint32_t index = 0; index < CATEGORY_NAMED; index++)
			if (nxu_streq(entry.name, g_categories[index])) category = index;

		if (entry.type == NXU_DIRENT_TYPE_DIRECTORY) {
			out->categories[category].bytes += walk(child, 1, &out->files_counted);
		} else {
			nxu_stat_t info;

			if (nxu_stat(child, &info) == 0) {
				out->categories[category].bytes += info.size;
				out->files_counted++;
			}
		}
	}
	(void)nxu_close((uint64_t)fd);

	nxu_service_copy(g_cache.mount, sizeof(g_cache.mount), mount);
	g_cache.usage = *out;
	g_cache.taken_us = nxu_uptime_us();
	log_line("measured ", mount);
	return 0;
}

static void
handle(nxu_service_message_t *message, const nxu_ipc_peer_t *peer)
{
	(void)peer;
	message->length = 0;

	switch (message->operation) {
	case NXU_STORAGED_VOLUME: {
		nxu_recovery_fs_mount_info_t mount;
		nxu_recovery_fs_space_info_t space;

		if (message->arguments[0] > UINT32_MAX) {
			message->status = (int32_t)-NXU_SYS_E_INVALID_ARGUMENT;
			break;
		}
		int64_t status = nxu_recovery_fs_mount_info((uint32_t)message->arguments[0], &mount);

		if (status < 0) {
			message->status = (int32_t)status;
			break;
		}

		nxu_volume_info_t info = {0};

		nxu_service_copy(info.path, sizeof(info.path), mount.path);
		nxu_service_copy(info.filesystem, sizeof(info.filesystem), mount.filesystem);
		nxu_service_copy(info.device, sizeof(info.device), mount.device);
		info.device_index = mount.device_index;
		info.read_only = mount.read_only;
		if (nxu_recovery_fs_space_info(mount.path, &space) == 0) {
			info.total_bytes = space.total_bytes;
			info.free_bytes = space.free_bytes;
		}
		memcpy(message->data, &info, sizeof(info));
		message->length = sizeof(info);
		break;
	}
	case NXU_STORAGED_USAGE: {
		nxu_storage_usage_t out;
		int64_t status = message->name[0] == '/' ? usage(message->name, &out) : -NXU_SYS_E_INVALID_ARGUMENT;

		message->status = (int32_t)status;
		if (status == 0) {
			memcpy(message->data, &out, sizeof(out));
			message->length = sizeof(out);
		}
		break;
	}
	default:
		message->status = (int32_t)-NXU_SYS_E_NOT_SUPPORTED;
		break;
	}
}

int
main(void)
{
	log_line("ready", 0);
	return nxu_service_run(NXU_STORAGED_SERVICE, handle, 0, 1000000U);
}
