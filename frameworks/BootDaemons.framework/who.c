/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * who -- who is logged in now, from utmp: the account, the terminal and
 * when the session began.
 *
 *     user      console   Sat Nov 14 14:20
 */
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/utmp.h>

#include <stdint.h>

static nxu_utmp_t g_current[NXU_UTMP_MAX];

static void
say(const char *text)
{
	(void)nxu_write(1, text, nxu_strlen(text));
}

static void
say_padded(const char *text, uint32_t width)
{
	uint64_t length = nxu_strlen(text);

	say(text);
	for (; length < width; length++)
		say(" ");
}

int
main(uint64_t argc, const char *const *argv)
{
	(void)argc;
	(void)argv;

	int64_t count = nxu_utmp_read(NXU_UTMP_PATH, g_current, NXU_UTMP_MAX, 0);
	int64_t offset = nxu_utmp_local_offset();

	if (count < 0) {
		say("who: cannot read " NXU_UTMP_PATH "\n");
		return 1;
	}

	for (int64_t index = 0; index < count; index++) {
		const nxu_utmp_t *record = &g_current[index];
		char when[20];

		if (record->ut_type != NXU_UT_USER_PROCESS) continue;

		say_padded(record->ut_user, 10U);
		say_padded(record->ut_line, 10U);
		nxu_utmp_format_time(record->ut_time, offset, false, when);
		say(when);
		say("\n");
	}

	return 0;
}
