/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * chown -- change a file's owner and/or group. Phase 3 of the native Unix
 * security substrate (doc/architecture/unix-security.md). The owner is given as
 * `uid`, `uid:gid`, or `:gid`, e.g. `chown 0:0 /Volumes/mainOS/etc/motd`.
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

/*
 * Parse a decimal id up to the end of the string or a ':' separator. On success
 * returns the value and advances *cursor past the digits; an empty field yields
 * NXU_ID_UNCHANGED (leave that id alone). A bad digit sets *ok = 0.
 */
static uint32_t
parse_id(const char **cursor, int *ok)
{
	const char *text = *cursor;
	*ok = 1;
	if (text[0] == '\0' || text[0] == ':') return NXU_ID_UNCHANGED;

	uint32_t value = 0U;
	while (text[0] != '\0' && text[0] != ':') {
		if (text[0] < '0' || text[0] > '9') {
			*ok = 0;
			return 0U;
		}
		value = value * 10U + (uint32_t)(text[0] - '0');
		text++;
	}
	*cursor = text;
	return value;
}

int
main(int argc, char **argv)
{
	if (argc < 3) {
		say("usage: chown <uid[:gid]> <path>\n");
		return 2;
	}

	const char *cursor = argv[1];
	int ok;
	uint32_t uid = parse_id(&cursor, &ok);
	uint32_t gid = NXU_ID_UNCHANGED;
	if (ok && cursor[0] == ':') {
		cursor++;
		gid = parse_id(&cursor, &ok);
	}
	if (!ok) {
		say("chown: invalid owner\n");
		return 2;
	}

	int64_t result = nxu_chown(argv[2], uid, gid);
	if (result < 0) {
		say("chown: ");
		say(argv[2]);
		say(result == -NXU_SYS_E_DENIED ? ": permission denied\n" : ": failed\n");
		return 1;
	}
	return 0;
}
