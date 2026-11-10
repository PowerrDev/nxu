/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * Account database reader: parse /etc/passwd and /etc/group (colon-separated,
 * one record per line) to resolve names and numeric IDs. The files are small,
 * so each lookup reads the whole file into a stack buffer and scans it.
 */
#include <nxu/pwdb.h>

#include <nxu/syscall.h>

#include <stdint.h>

#define PWDB_BUFFER 4096U

/* Read a whole file into buffer (NUL-terminated). Returns bytes read, or -1. */
static int64_t
pwdb_slurp(const char *path, char *buffer, uint64_t capacity)
{
	int64_t fd = nxu_open(path, NXU_O_READ);
	if (fd < 0) return -1;

	uint64_t total = 0ULL;
	while (total + 1ULL < capacity) {
		int64_t count = nxu_read((uint64_t)fd, buffer + total, capacity - 1ULL - total);
		if (count <= 0) break;
		total += (uint64_t)count;
	}
	(void)nxu_close((uint64_t)fd);
	buffer[total] = '\0';
	return (int64_t)total;
}

static uint32_t
pwdb_atou(const char *text, uint32_t length)
{
	uint32_t value = 0U;
	for (uint32_t i = 0U; i < length; i++) {
		if (text[i] < '0' || text[i] > '9') break;
		value = value * 10U + (uint32_t)(text[i] - '0');
	}
	return value;
}

static int
pwdb_field_equals(const char *field, uint32_t length, const char *name)
{
	uint32_t i = 0U;
	for (; i < length; i++) {
		if (name[i] == '\0' || field[i] != name[i]) return 0;
	}
	return name[i] == '\0';
}

static void
pwdb_copy_field(char *dst, uint32_t capacity, const char *field, uint32_t length)
{
	uint32_t i = 0U;
	for (; i < length && i + 1U < capacity; i++) dst[i] = field[i];
	dst[i] = '\0';
}

/*
 * Split one line (the region from `line` up to '\n' or NUL) into up to `max`
 * colon-separated fields. Returns the field count and advances *next to the
 * start of the following line. The fields point into the buffer.
 */
static int
pwdb_split(char *line, const char **fields, uint32_t *lengths, int max, char **next)
{
	int count = 0;
	char *field_begin = line;
	char *p = line;
	for (; *p != '\0' && *p != '\n'; p++) {
		if (*p == ':') {
			if (count < max) {
				fields[count] = field_begin;
				lengths[count] = (uint32_t)(p - field_begin);
				count++;
			}
			field_begin = p + 1;
		}
	}
	if (count < max) {
		fields[count] = field_begin;
		lengths[count] = (uint32_t)(p - field_begin);
		count++;
	}
	*next = (*p == '\n') ? p + 1 : p;
	return count;
}

static int
pwdb_passwd_lookup(const char *name, uint32_t uid, int by_name, nxu_passwd_t *out)
{
	if (out == 0) return -1;

	char buffer[PWDB_BUFFER];
	if (pwdb_slurp(NXU_PASSWD_PATH, buffer, sizeof(buffer)) < 0) return -1;

	char *line = buffer;
	while (*line != '\0') {
		const char *fields[7];
		uint32_t lengths[7];
		char *next;
		int count = pwdb_split(line, fields, lengths, 7, &next);
		line = next;
		if (count < 4) continue;

		uint32_t record_uid = pwdb_atou(fields[2], lengths[2]);
		int match = by_name ? pwdb_field_equals(fields[0], lengths[0], name) : (record_uid == uid);
		if (!match) continue;

		pwdb_copy_field(out->pw_name, sizeof(out->pw_name), fields[0], lengths[0]);
		out->pw_uid = record_uid;
		out->pw_gid = pwdb_atou(fields[3], lengths[3]);
		if (count > 5)
			pwdb_copy_field(out->pw_dir, sizeof(out->pw_dir), fields[5], lengths[5]);
		else
			out->pw_dir[0] = '\0';
		if (count > 6)
			pwdb_copy_field(out->pw_shell, sizeof(out->pw_shell), fields[6], lengths[6]);
		else
			out->pw_shell[0] = '\0';
		return 0;
	}
	return -1;
}

int
nxu_getpwuid(uint32_t uid, nxu_passwd_t *out)
{
	return pwdb_passwd_lookup(0, uid, 0, out);
}

int
nxu_getpwnam(const char *name, nxu_passwd_t *out)
{
	if (name == 0) return -1;
	return pwdb_passwd_lookup(name, 0U, 1, out);
}

int
nxu_getgrgid(uint32_t gid, nxu_group_t *out)
{
	if (out == 0) return -1;

	char buffer[PWDB_BUFFER];
	if (pwdb_slurp(NXU_GROUP_PATH, buffer, sizeof(buffer)) < 0) return -1;

	char *line = buffer;
	while (*line != '\0') {
		const char *fields[4];
		uint32_t lengths[4];
		char *next;
		int count = pwdb_split(line, fields, lengths, 4, &next);
		line = next;
		if (count < 3) continue;

		if (pwdb_atou(fields[2], lengths[2]) != gid) continue;

		pwdb_copy_field(out->gr_name, sizeof(out->gr_name), fields[0], lengths[0]);
		out->gr_gid = gid;
		return 0;
	}
	return -1;
}
