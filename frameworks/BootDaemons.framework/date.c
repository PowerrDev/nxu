/*
 * date -- the local date and time from timed, e.g.
 * "Fri Nov 13 17:42:05 WET 2026". `date -u` shows UTC; `date -z` lists the
 * time zones timed knows; `date -z ZONE` (as root) changes the zone.
 */
#include <nxu/services.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

static void
say(const char *text)
{
	(void)nxu_write(1, text, nxu_strlen(text));
}

static void
two(uint64_t value)
{
	char digits[3] = {(char)('0' + value / 10U % 10U), (char)('0' + value % 10U), 0};

	say(digits);
}

static void
number(int64_t value)
{
	char reversed[24], out[24];
	uint32_t length = 0, at = 0;
	uint64_t magnitude = (uint64_t)(value < 0 ? -value : value);

	do {
		reversed[length++] = (char)('0' + magnitude % 10U);
		magnitude /= 10U;
	} while (magnitude);
	if (value < 0) out[at++] = '-';
	while (length) out[at++] = reversed[--length];
	out[at] = 0;
	say(out);
}

static void
show(int64_t seconds, const char *zone)
{
	static const char *const weekdays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
	static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
										 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	int64_t days = seconds / 86400 - (seconds % 86400 < 0);
	int64_t day_seconds = seconds - days * 86400;
	int64_t z = days + 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	uint64_t doe = (uint64_t)(z - era * 146097);
	uint64_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
	uint64_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
	uint64_t mp = (5U * doy + 2U) / 153U;
	uint64_t day = doy - (153U * mp + 2U) / 5U + 1U;
	uint64_t month = mp < 10U ? mp + 3U : mp - 9U;
	int64_t year = (int64_t)yoe + era * 400 + (month <= 2U);
	int64_t weekday = days % 7;

	say(weekdays[weekday < 0 ? weekday + 7 : weekday]);
	say(" ");
	say(months[month - 1U]);
	say(" ");
	two(day);
	say(" ");
	two((uint64_t)day_seconds / 3600U);
	say(":");
	two((uint64_t)day_seconds / 60U % 60U);
	say(":");
	two((uint64_t)day_seconds % 60U);
	say(" ");
	say(zone);
	say(" ");
	number(year);
	say("\n");
}

int
main(uint64_t argc, const char *const *argv)
{
	if (argc == 2 && nxu_streq(argv[1], "-z")) {
		nxu_time_zone_t zone;

		for (uint32_t index = 0; nxu_timed_zone(index, &zone) == 0; index++) {
			say(zone.name);
			say("  (");
			say(zone.city);
			say(")\n");
		}
		return 0;
	}
	if (argc == 3 && nxu_streq(argv[1], "-z")) {
		int64_t status = nxu_timed_set_zone(argv[2]);

		if (status == -NXU_SYS_E_DENIED) say("date: changing the time zone needs root (sudo)\n");
		else if (status == -NXU_SYS_E_NOT_FOUND) say("date: unknown time zone (see date -z)\n");
		else if (status < 0) say("date: timed is not answering\n");
		return status < 0 ? 1 : 0;
	}

	bool utc = argc == 2 && nxu_streq(argv[1], "-u");
	nxu_time_info_t info;

	if (nxu_timed_now(&info) < 0) {
		/* No timed: the RTC alone is UTC. */
		show(nxu_time(), "UTC");
		return 0;
	}
	show(utc ? info.utc_seconds : info.local_seconds, utc ? "UTC" : info.abbreviation);
	return 0;
}
