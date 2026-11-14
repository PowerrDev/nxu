/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * last -- the logins recorded in wtmp, newest first, as BSD's last shows
 * them:
 *
 *     user      console   Sat Nov 14 14:20 - 14:21  (00:01)
 *     root      console   Sat Nov 14 14:22   still logged in
 *
 *     wtmp begins Sat Nov 14 14:18
 *
 * `last <user>` shows only that account's. A login's end is the next
 * logout on the same terminal; with none, it is still logged in when utmp
 * says so, and otherwise ended without a logout record ("gone - no logout",
 * a crash or a power cut).
 */
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/utmp.h>

#include <stdbool.h>
#include <stdint.h>

#define LAST_RECORDS 512U
#define LAST_LINES 8U

static nxu_utmp_t g_records[LAST_RECORDS];
static nxu_utmp_t g_current[NXU_UTMP_MAX];

static void
say(const char *text)
{
	(void)nxu_write(1, text, nxu_strlen(text));
}

/* `text` and spaces to `width`. */
static void
say_padded(const char *text, uint32_t width)
{
	uint64_t length = nxu_strlen(text);

	say(text);
	for (; length < width; length++)
		say(" ");
}

static void
say_two(uint64_t value)
{
	char digits[3] = {(char)('0' + value / 10U % 10U), (char)('0' + value % 10U), 0};

	say(digits);
}

static void
say_number(uint64_t value)
{
	char reversed[24];
	char out[24];
	uint32_t length = 0U;
	uint32_t at = 0U;

	do {
		reversed[length++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0U);
	while (length != 0U)
		out[at++] = reversed[--length];
	out[at] = 0;
	say(out);
}

/* "(hh:mm)", or "(d+hh:mm)" past a day. */
static void
say_duration(int64_t seconds)
{
	uint64_t total = seconds < 0 ? 0U : (uint64_t)seconds;

	say("  (");
	if (total >= 86400U) {
		say_number(total / 86400U);
		say("+");
	}
	say_two(total / 3600U % 24U);
	say(":");
	say_two(total / 60U % 60U);
	say(")");
}

/* Whether utmp still has `login` as the session on its terminal. */
static bool
still_logged_in(const nxu_utmp_t *login, int64_t current)
{
	for (int64_t index = 0; index < current; index++) {
		const nxu_utmp_t *now = &g_current[index];

		if (now->ut_type == NXU_UT_USER_PROCESS && nxu_streq(now->ut_line, login->ut_line) &&
			now->ut_pid == login->ut_pid && now->ut_time == login->ut_time)
			return true;
	}

	return false;
}

int
main(uint64_t argc, const char *const *argv)
{
	const char *only = argc >= 2U ? argv[1] : 0;
	int64_t count = nxu_utmp_read(NXU_WTMP_PATH, g_records, LAST_RECORDS, 0);
	int64_t current = nxu_utmp_read(NXU_UTMP_PATH, g_current, NXU_UTMP_MAX, 0);
	int64_t offset = nxu_utmp_local_offset();

	if (count < 0) {
		say("last: cannot read " NXU_WTMP_PATH "\n");
		return 1;
	}

	/* Walking back in time: the logout last seen on each terminal is the end of the login before it. */
	char lines[LAST_LINES][NXU_UT_LINESIZE];
	int64_t ends[LAST_LINES];
	uint32_t line_count = 0U;

	for (int64_t index = count - 1; index >= 0; index--) {
		const nxu_utmp_t *record = &g_records[index];
		uint32_t slot = 0U;

		while (slot < line_count && !nxu_streq(lines[slot], record->ut_line))
			slot++;

		if (record->ut_type == NXU_UT_DEAD_PROCESS) {
			if (slot == line_count && line_count < LAST_LINES) {
				memcpy(lines[slot], record->ut_line, NXU_UT_LINESIZE);
				line_count++;
			}
			if (slot < line_count) ends[slot] = record->ut_time;
			continue;
		}

		if (record->ut_type != NXU_UT_USER_PROCESS) continue;

		int64_t end = slot < line_count ? ends[slot] : -1;

		/* This login's end is used up: an earlier login on the terminal ended at an earlier logout. */
		if (slot < line_count) ends[slot] = -1;

		if (only != 0 && !nxu_streq(record->ut_user, only)) continue;

		char when[20];

		say_padded(record->ut_user, 10U);
		say_padded(record->ut_line, 10U);
		nxu_utmp_format_time(record->ut_time, offset, false, when);
		say(when);

		if (end >= 0) {
			say(" - ");
			nxu_utmp_format_time(end, offset, true, when);
			say(when);
			say_duration(end - record->ut_time);
		} else if (still_logged_in(record, current)) {
			say("   still logged in");
		} else {
			say("   gone - no logout");
		}
		say("\n");
	}

	if (count > 0) {
		char when[20];

		nxu_utmp_format_time(g_records[0].ut_time, offset, false, when);
		say("\nwtmp begins ");
		say(when);
		say("\n");
	}

	return 0;
}
