/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * chmod -- change a file's permission bits. Mode is given in octal, e.g.
 * `chmod 644 /Volumes/mainOS/etc/motd`.
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

/* Parse an octal mode string; returns 0 and sets *ok=0 on a bad digit. */
static uint32_t
parse_octal(const char *text, int *ok)
{
	uint32_t value = 0U;
	*ok = 0;
	if (text[0] == '\0') return 0U;
	for (uint64_t i = 0ULL; text[i] != '\0'; i++) {
		if (text[i] < '0' || text[i] > '7') return 0U;
		value = (value << 3U) | (uint32_t)(text[i] - '0');
	}
	*ok = 1;
	return value;
}

int
main(uint64_t argc, const char *const *argv)
{
	if (argc < 3U) {
		say("usage: chmod <octal-mode> <path>\n");
		return 2;
	}

	int ok;
	uint32_t mode = parse_octal(argv[1], &ok);
	if (!ok) {
		say("chmod: invalid mode\n");
		return 2;
	}

	int64_t result = nxu_chmod(argv[2], mode);
	if (result < 0) {
		say("chmod: ");
		say(argv[2]);
		say(result == -NXU_SYS_E_DENIED ? ": permission denied\n" : ": failed\n");
		return 1;
	}
	return 0;
}
