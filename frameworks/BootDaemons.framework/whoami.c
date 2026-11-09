/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * whoami -- print the effective user. Part of Phase 2 of the native Unix
 * security substrate (doc/architecture/unix-security.md). Until an account
 * database exists (Phase 5) there is no name lookup, so root prints as "root"
 * and any other effective UID prints as "uid <n>".
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

int
main(void)
{
	int64_t euid = nxu_geteuid();
	if (euid < 0) {
		say("whoami: cannot read credentials\n");
		return 1;
	}
	if (euid == 0) {
		say("root\n");
		return 0;
	}
	say("uid ");
	say_u32((uint32_t)euid);
	say("\n");
	return 0;
}
