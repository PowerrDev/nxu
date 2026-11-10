/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * id -- print real and effective user and group IDs, and supplementary groups,
 * each annotated with its name from /etc/passwd and /etc/group when known.
 */
#include <nxu/syscall.h>
#include <nxu/pwdb.h>

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
	if (value < 0) {
		say("?");
		return;
	}
	say_u32((uint32_t)value);
}

/* Append "(name)" for a user id, if /etc/passwd knows it. */
static void
user_name(int64_t value)
{
	if (value < 0) return;
	nxu_passwd_t pw;
	if (nxu_getpwuid((uint32_t)value, &pw) == 0) {
		say("(");
		say(pw.pw_name);
		say(")");
	}
}

/* Append "(name)" for a group id, if /etc/group knows it. */
static void
group_name(int64_t value)
{
	if (value < 0) return;
	nxu_group_t gr;
	if (nxu_getgrgid((uint32_t)value, &gr) == 0) {
		say("(");
		say(gr.gr_name);
		say(")");
	}
}

int
main(void)
{
	int64_t uid = nxu_getuid();
	int64_t gid = nxu_getgid();
	field("uid=", uid);
	user_name(uid);
	field(" gid=", gid);
	group_name(gid);
	field(" euid=", nxu_geteuid());
	user_name(nxu_geteuid());
	field(" egid=", nxu_getegid());
	group_name(nxu_getegid());

	uint32_t groups[16];
	int64_t count = nxu_getgroups(16U, groups);
	if (count > 0) {
		say(" groups=");
		for (int64_t i = 0; i < count; i++) {
			if (i != 0) say(",");
			say_u32(groups[i]);
			group_name((int64_t)groups[i]);
		}
	}
	say("\n");
	return 0;
}
