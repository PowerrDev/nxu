/*
 * loadtest -- keep a CPU busy, to see how the user interface holds up under
 * load (the desktop logs frames it misses; see ui-service-nxu's budget.rs).
 *
 *   loadtest [SECONDS]
 *
 * Spins for SECONDS (default 30) doing plain arithmetic, never blocking or
 * yielding: the worst case for a scheduler, the way a picture decode or a
 * disk walk looks to it. Run several for more load.
 */
#include <nxu/string.h>
#include <nxu/syscall.h>

static uint32_t
seconds_from(const char *text, uint32_t fallback)
{
	uint32_t value = 0;

	if (!text || !*text) return fallback;
	for (; *text; text++) {
		if (*text < '0' || *text > '9') return fallback;
		value = value * 10U + (uint32_t)(*text - '0');
	}
	return value;
}

static void
say(const char *text)
{
	(void)nxu_write(1, text, nxu_strlen(text));
}

int
main(uint64_t argc, const char *const *argv)
{
	uint32_t seconds = argc > 1 ? seconds_from(argv[1], 30) : 30;
	int64_t until = nxu_uptime_us() + (int64_t)seconds * 1000000;
	volatile uint32_t sink = 1;

	say("loadtest: spinning\n");
	while (nxu_uptime_us() < until) {
		for (uint32_t index = 0; index < 100000U; index++) sink = sink * 1664525U + 1013904223U;
	}
	say("loadtest: done\n");
	return 0;
}
