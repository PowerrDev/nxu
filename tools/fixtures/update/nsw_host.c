/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <frameworks/SoftwareUpdate.framework/include/nsw.h>
#include <nxu/syscall.h>

static uint64_t g_written;
static bool g_fail_write;
static bool g_fail_sync;
static bool g_corrupt;
static const char *g_recovery_root;

static const char *
host_path(const char *path)
{
	static char translated[1024];
	if (g_recovery_root != NULL && strncmp(path, "/disk/", 6) == 0) {
		(void)snprintf(translated, sizeof(translated), "%s/SystemVolume%s", g_recovery_root, path + 5);
		return translated;
	}
	const char *prefix = NSW_RECOVERY_ROOT;
	uint32_t length = strlen(prefix);
	if (g_recovery_root != NULL && strncmp(path, prefix, length) == 0 && (path[length] == '/' || path[length] == 0)) {
		(void)snprintf(translated, sizeof(translated), "%s%s", g_recovery_root, path + length);
		return translated;
	}
	return path;
}

uint64_t
nxu_strlen(const char *text)
{
	return strlen(text);
}

bool
nxu_streq(const char *left, const char *right)
{
	return strcmp(left, right) == 0;
}

static int64_t
host_error(void)
{
	return errno == ENOENT ? -NXU_SYS_E_NOT_FOUND : errno == EEXIST ? -NXU_SYS_E_EXISTS : -NXU_SYS_E_IO;
}

int64_t
nxu_stat(const char *path, nxu_stat_t *info)
{
	struct stat status;
	if (stat(host_path(path), &status) != 0) {
		return host_error();
	}
	info->size = status.st_size;
	info->type = S_ISDIR(status.st_mode) ? NXU_STAT_TYPE_DIRECTORY : NXU_STAT_TYPE_REGULAR;
	return 0;
}

int64_t
nxu_open(const char *path, uint64_t flags)
{
	int mode = (flags & NXU_O_WRITE) ? O_WRONLY : O_RDONLY;
	if (flags & NXU_O_CREATE) {
		mode |= O_CREAT;
	}
	if (flags & NXU_O_TRUNCATE) {
		mode |= O_TRUNC;
	}
	int descriptor = open(host_path(path), mode, 0600);
	return descriptor < 0 ? host_error() : descriptor;
}

int64_t
nxu_close(uint64_t descriptor)
{
	return close(descriptor) == 0 ? 0 : host_error();
}

int64_t
nxu_seek(uint64_t descriptor, uint64_t offset)
{
	return lseek(descriptor, offset, SEEK_SET) < 0 ? host_error() : (int64_t)offset;
}

int64_t
nxu_read(uint64_t descriptor, void *buffer, uint64_t length)
{
	if (length > 4093) {
		length = 4093;
	}
	ssize_t count = read(descriptor, buffer, length);
	return count < 0 ? host_error() : count;
}

int64_t
nxu_write(uint64_t descriptor, const void *buffer, uint64_t length)
{
	if (descriptor > 2 && g_fail_write && g_written >= 16384) {
		return -NXU_SYS_E_IO;
	}
	if (length > 4091) {
		length = 4091;
	}
	char copy[4096];
	if (g_corrupt && descriptor > 2 && g_written == 0 && length > 100) {
		memcpy(copy, buffer, length);
		copy[100] ^= 1;
		buffer = copy;
	}
	ssize_t count = write(descriptor, buffer, length);
	if (descriptor > 2 && count > 0) {
		g_written += count;
	}
	return count < 0 ? host_error() : count;
}

int64_t
nxu_mkdir(const char *path)
{
	return mkdir(host_path(path), 0700) == 0 ? 0 : host_error();
}

int64_t
nxu_unlink(const char *path)
{
	return unlink(host_path(path)) == 0 ? 0 : host_error();
}

int64_t
nxu_rmdir(const char *path)
{
	return rmdir(host_path(path)) == 0 ? 0 : host_error();
}

int64_t
nxu_sync(void)
{
	return g_fail_sync ? -NXU_SYS_E_IO : 0;
}

int64_t
nxu_yield(void)
{
	return 0;
}

int64_t
nxu_uptime_us(void)
{
	return 123456789;
}

int
main(int argc, char **argv)
{
	if (argc < 3) {
		return 2;
	}
	if (argc > 3 && (strcmp(argv[3], "schedule") == 0 || strcmp(argv[3], "confirm") == 0 ||
					 strcmp(argv[3], "schedule-sync") == 0)) {
		g_recovery_root = argv[2];
		g_fail_sync = strcmp(argv[3], "schedule-sync") == 0;
		return (strcmp(argv[3], "confirm") == 0 ? nsw_confirm_boot() : nsw_schedule(argv[1])) ? 0 : 1;
	}
	g_fail_write = argc > 3 && strcmp(argv[3], "write") == 0;
	g_corrupt = argc > 3 && strcmp(argv[3], "corrupt") == 0;
	g_fail_sync = argc > 3 && strcmp(argv[3], "sync") == 0;
	return nsw_prepare(argv[1], argv[2]) ? 0 : 1;
}
