/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * id -- print real and effective user and group IDs, and supplementary groups.
 * Names arrive once an account database exists; for now the IDs are numeric.
 */
#include <nxu/syscall.h>

#include <stdint.h>

static void
say(const char *text)
{
	uint64_t length = 0ULL;
	while (text[length] != '\0')
		length++;
	(void)nxu_write(1ULL, text, length);
}

static void
say_u32(uint32_t value)
{
	char digits[11];
	int index = 10;
	digits[index] = '\0';
	if (value == 0U) {
		say("0");
		return;
	}
	while (value != 0U) {
		digits[--index] = (char)('0' + (value % 10U));
		value /= 10U;
	}
	say(&digits[index]);
}

static void
field(const char *label, int64_t value)
{
	say(label);
	say(value < 0 ? "?" : "");
	if (value >= 0) say_u32((uint32_t)value);
}

int
main(void)
{
	field("uid=", nxu_getuid());
	field(" gid=", nxu_getgid());
	field(" euid=", nxu_geteuid());
	field(" egid=", nxu_getegid());

	uint32_t groups[16];
	int64_t count = nxu_getgroups(16U, groups);
	if (count > 0) {
		say(" groups=");
		for (int64_t i = 0; i < count; i++) {
			if (i != 0) say(",");
			say_u32(groups[i]);
		}
	}
	say("\n");
	return 0;
}
