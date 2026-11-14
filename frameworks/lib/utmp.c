/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * utmp and wtmp; see nxu/utmp.h.
 *
 * utmp is small (a record per terminal, at most NXU_UTMP_MAX), so it is
 * read whole, changed in memory and written back. wtmp only grows: records
 * are appended, and readers keep the last ones they were asked for as they
 * read it from the start.
 */
#include <nxu/services.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/utmp.h>

#include <stdbool.h>
#include <stdint.h>

#define UTMP_FILE_MODE 0644U

/* A terminal's line: its name in /dev, from either that name or its path. */
static void
utmp_line_of(const char *terminal, char line[NXU_UT_LINESIZE])
{
	const char *name = terminal;
	const char *prefix = "/dev/";
	uint32_t index = 0U;

	while (prefix[index] != '\0' && terminal[index] == prefix[index])
		index++;
	if (prefix[index] == '\0') name = terminal + index;

	memset(line, 0, NXU_UT_LINESIZE);
	for (index = 0U; name[index] != '\0' && index + 1U < NXU_UT_LINESIZE; index++)
		line[index] = name[index];
}

static void
utmp_copy(char *out, uint32_t capacity, const char *text)
{
	memset(out, 0, capacity);
	for (uint32_t index = 0U; text[index] != '\0' && index + 1U < capacity; index++)
		out[index] = text[index];
}

static bool
utmp_write_all(int64_t descriptor, const void *bytes, uint64_t size)
{
	const uint8_t *data = bytes;

	while (size != 0U) {
		int64_t written = nxu_write((uint64_t)descriptor, data, size);

		if (written <= 0) return false;
		data += written;
		size -= (uint64_t)written;
	}

	return true;
}

/* Read one record; false at the end of the file (or a short record left by a crash). */
static bool
utmp_read_record(int64_t descriptor, nxu_utmp_t *record)
{
	uint8_t *data = (uint8_t *)record;
	uint64_t done = 0U;

	while (done < sizeof(*record)) {
		int64_t count = nxu_read((uint64_t)descriptor, data + done, sizeof(*record) - done);

		if (count <= 0) return false;
		done += (uint64_t)count;
	}

	return true;
}

int64_t
nxu_utmp_read(const char *path, nxu_utmp_t *records, uint32_t capacity, uint32_t *total)
{
	if (total != 0) *total = 0U;

	int64_t descriptor = nxu_open(path, NXU_O_READ);

	if (descriptor == -NXU_SYS_E_NOT_FOUND) return 0;
	if (descriptor < 0) return -1;

	/* A ring of the last `capacity`, put in order at the end. */
	uint32_t count = 0U;
	uint32_t next = 0U;
	nxu_utmp_t record;

	while (capacity != 0U && utmp_read_record(descriptor, &record)) {
		records[next] = record;
		next = (next + 1U) % capacity;
		count++;
	}

	(void)nxu_close((uint64_t)descriptor);

	if (total != 0) *total = count;
	if (count <= capacity) return (int64_t)count;

	/* Wrapped: rotate so the oldest kept is first. */
	for (uint32_t turn = 0U; turn < next; turn++) {
		nxu_utmp_t first = records[0];

		memmove(records, records + 1, (capacity - 1U) * sizeof(*records));
		records[capacity - 1U] = first;
	}

	return (int64_t)capacity;
}

/* Set `line`'s record in utmp to `record`, adding one if the terminal has none. */
static int
utmp_set(const nxu_utmp_t *record)
{
	static nxu_utmp_t table[NXU_UTMP_MAX];
	int64_t count = nxu_utmp_read(NXU_UTMP_PATH, table, NXU_UTMP_MAX, 0);

	if (count < 0) count = 0;

	int64_t slot = -1;

	for (int64_t index = 0; index < count && slot < 0; index++)
		if (nxu_streq(table[index].ut_line, record->ut_line)) slot = index;

	if (slot < 0) {
		if (count >= (int64_t)NXU_UTMP_MAX) return -1;
		slot = count++;
	}

	table[slot] = *record;

	int64_t descriptor = nxu_open(NXU_UTMP_PATH, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);

	if (descriptor < 0) return -1;

	bool written = utmp_write_all(descriptor, table, (uint64_t)count * sizeof(table[0]));

	(void)nxu_close((uint64_t)descriptor);
	(void)nxu_chmod(NXU_UTMP_PATH, UTMP_FILE_MODE);
	return written ? 0 : -1;
}

static int
utmp_append_wtmp(const nxu_utmp_t *record)
{
	int64_t descriptor = nxu_open(NXU_WTMP_PATH, NXU_O_WRITE | NXU_O_CREATE | NXU_O_APPEND);

	if (descriptor < 0) return -1;

	bool written = utmp_write_all(descriptor, record, sizeof(*record));

	(void)nxu_close((uint64_t)descriptor);
	(void)nxu_chmod(NXU_WTMP_PATH, UTMP_FILE_MODE);
	return written ? 0 : -1;
}

int
nxu_utmp_login(const char *line, const char *user, uint32_t pid, int64_t time)
{
	nxu_utmp_t record;

	memset(&record, 0, sizeof(record));
	record.ut_type = NXU_UT_USER_PROCESS;
	record.ut_pid = pid;
	record.ut_time = time;
	utmp_line_of(line, record.ut_line);
	utmp_copy(record.ut_user, sizeof(record.ut_user), user);

	int set = utmp_set(&record);
	int appended = utmp_append_wtmp(&record);

	return set == 0 && appended == 0 ? 0 : -1;
}

int
nxu_utmp_logout(const char *line, int64_t time)
{
	nxu_utmp_t record;

	memset(&record, 0, sizeof(record));
	record.ut_type = NXU_UT_DEAD_PROCESS;
	record.ut_time = time;
	utmp_line_of(line, record.ut_line);

	int set = utmp_set(&record);
	int appended = utmp_append_wtmp(&record);

	return set == 0 && appended == 0 ? 0 : -1;
}

bool
nxu_utmp_active(const char *terminal)
{
	static nxu_utmp_t table[NXU_UTMP_MAX];
	char line[NXU_UT_LINESIZE];
	int64_t count = nxu_utmp_read(NXU_UTMP_PATH, table, NXU_UTMP_MAX, 0);

	utmp_line_of(terminal, line);

	for (int64_t index = 0; index < count; index++)
		if (nxu_streq(table[index].ut_line, line)) return table[index].ut_type == NXU_UT_USER_PROCESS;

	return false;
}

bool
nxu_utmp_clear(const char *terminal)
{
	if (!nxu_utmp_active(terminal)) return false;

	nxu_utmp_t record;

	memset(&record, 0, sizeof(record));
	record.ut_type = NXU_UT_DEAD_PROCESS;
	record.ut_time = nxu_time();
	utmp_line_of(terminal, record.ut_line);
	(void)utmp_set(&record);
	return true;
}

bool
nxu_utmp_last_login(const char *user, nxu_utmp_t *record)
{
	static nxu_utmp_t recent[256];
	int64_t count = nxu_utmp_read(NXU_WTMP_PATH, recent, 256U, 0);

	for (int64_t index = count - 1; index >= 0; index--) {
		if (recent[index].ut_type == NXU_UT_USER_PROCESS && nxu_streq(recent[index].ut_user, user)) {
			*record = recent[index];
			return true;
		}
	}

	return false;
}

/* timed's bootd plist: only a system that runs timed is asked for local time. */
#define UTMP_TIMED_PLIST "/Volumes/mainOS/System/Library/BootDaemons/com.nxu.timed.plist"

int64_t
nxu_utmp_local_offset(void)
{
	nxu_time_info_t info;
	nxu_stat_t plist;

	/* A lookup of a service that is never started waits for it for minutes: without timed, UTC. */
	if (nxu_stat(UTMP_TIMED_PLIST, &plist) != 0) return 0;

	return nxu_timed_now(&info) < 0 ? 0 : (int64_t)info.offset_seconds;
}

static void
utmp_two(char *out, uint64_t value)
{
	out[0] = (char)('0' + value / 10U % 10U);
	out[1] = (char)('0' + value % 10U);
}

void
nxu_utmp_format_time(int64_t utc, int64_t offset, bool short_form, char *out)
{
	static const char *const weekdays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
	static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
										 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	int64_t seconds = utc + offset;
	int64_t days = seconds / 86400 - (seconds % 86400 < 0);
	int64_t day_seconds = seconds - days * 86400;

	/* Civil date from a day count (Howard Hinnant's algorithm, as date(1) here uses). */
	int64_t z = days + 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	uint64_t doe = (uint64_t)(z - era * 146097);
	uint64_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
	uint64_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
	uint64_t mp = (5U * doy + 2U) / 153U;
	uint64_t day = doy - (153U * mp + 2U) / 5U + 1U;
	uint64_t month = mp < 10U ? mp + 3U : mp - 9U;
	int64_t weekday = days % 7;
	uint32_t at = 0U;

	if (!short_form) {
		const char *name = weekdays[weekday < 0 ? weekday + 7 : weekday];

		memcpy(out, name, 3U);
		out[3] = ' ';
		memcpy(out + 4, months[month - 1U], 3U);
		out[7] = ' ';
		out[8] = day < 10U ? ' ' : (char)('0' + day / 10U);
		out[9] = (char)('0' + day % 10U);
		out[10] = ' ';
		at = 11U;
	}

	utmp_two(out + at, (uint64_t)day_seconds / 3600U);
	out[at + 2U] = ':';
	utmp_two(out + at + 3U, (uint64_t)day_seconds / 60U % 60U);
	out[at + 5U] = '\0';
}
