/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * stat -- show a file's type, mode, owner and size. Phase 3 of the native Unix
 * security substrate (doc/architecture/unix-security.md): the first tool that
 * surfaces file ownership and mode bits, now that the VFS reports them.
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

/* Print mode as a 6-digit octal (e.g. 100644), the way the on-disk mode reads. */
static void
say_octal(uint32_t value)
{
	char digits[12];
	int index = 11;
	digits[index] = '\0';
	do {
		digits[--index] = (char)('0' + (value & 7U));
		value >>= 3U;
	} while (value != 0U);
	say(&digits[index]);
}

static const char *
type_name(uint32_t type)
{
	switch (type) {
	case NXU_STAT_TYPE_REGULAR:
		return "regular";
	case NXU_STAT_TYPE_DIRECTORY:
		return "directory";
	case NXU_STAT_TYPE_CHARACTER:
		return "character";
	case NXU_STAT_TYPE_BLOCK:
		return "block";
	default:
		return "unknown";
	}
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		say("usage: stat <path>\n");
		return 2;
	}

	nxu_stat_t st;
	int64_t result = nxu_stat(argv[1], &st);
	if (result < 0) {
		say("stat: cannot stat ");
		say(argv[1]);
		say("\n");
		return 1;
	}

	say(argv[1]);
	say("\n  type ");
	say(type_name(st.type));
	say("  mode 0");
	say_octal(st.mode);
	say("\n  uid ");
	say_u32(st.uid);
	say("  gid ");
	say_u32(st.gid);
	say("  nlink ");
	say_u32(st.nlink);
	say("\n  size ");
	say_u32((uint32_t)st.size);
	say("  inode ");
	say_u32((uint32_t)st.inode);
	say("\n");
	return 0;
}
