/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        tools/vfs/host/test_pathname.c
 *
 * Host test of path resolution (vfs/pathname.c), built with the sanitizers
 * by tools/vfs/test_host.sh: relative and absolute paths, ".", "..",
 * repeated and trailing slashes, the root, and results that do not fit.
 */

#include <vfs/pathname.h>

#include <stdio.h>
#include <string.h>

static unsigned g_failures;

static void
expect(const char *base, const char *path, const char *wanted)
{
	char out[256];
	bool ok = vfs_pathname_resolve(base, path, out, sizeof(out));

	if (wanted == 0 ? ok : !ok || strcmp(out, wanted) != 0) {
		fprintf(stderr, "FAIL base \"%s\" path \"%s\": got %s%s%s, wanted %s\n", base ? base : "(null)", path,
				ok ? "\"" : "", ok ? out : "failure", ok ? "\"" : "", wanted ? wanted : "failure");
		g_failures++;
	}
}

int
main(void)
{
	expect("/", "/", "/");
	expect("/Volumes/mainOS", "etc/passwd", "/Volumes/mainOS/etc/passwd");
	expect("/Volumes/mainOS", "/dev/console", "/dev/console");
	expect("/Volumes/mainOS/", "./etc//./passwd/", "/Volumes/mainOS/etc/passwd");
	expect("/Volumes/mainOS", "..", "/Volumes");
	expect("/Volumes/mainOS", "../..", "/");
	expect("/Volumes/mainOS", "../../../..", "/");
	expect("/a/b/c", "../../x/../y", "/a/y");
	expect("/", "..", "/");
	expect("/", ".", "/");
	expect("/a", "", 0);
	expect(0, "relative", 0);
	expect("not/absolute", "x", 0);
	expect("/a", "///b", "/b");
	expect("/a", "b/..", "/a");
	expect("/a", "...", "/a/...");
	expect("/a", "..b", "/a/..b");

	/* Fitting exactly, and one byte over. */
	{
		char name[255];
		char out[256];

		memset(name, 'n', sizeof(name) - 1U);
		name[sizeof(name) - 1U] = '\0';
		if (!vfs_pathname_resolve("/", name, out, sizeof(out)) || strlen(out) != 255U) {
			fprintf(stderr, "FAIL a 255-byte path should fit in 256\n");
			g_failures++;
		}
		if (vfs_pathname_resolve("/x", name, out, sizeof(out))) {
			fprintf(stderr, "FAIL a 257-byte path should not fit in 256\n");
			g_failures++;
		}
		/* Too long on the way, short in the end: still refused, never written past the end. */
		if (vfs_pathname_resolve("/", "a", out, 1U)) {
			fprintf(stderr, "FAIL capacity 1 cannot hold a path\n");
			g_failures++;
		}
	}

	if (g_failures != 0U) {
		fprintf(stderr, "pathname: %u failure(s)\n", g_failures);
		return 1;
	}

	printf("pathname: all cases passed\n");
	return 0;
}
