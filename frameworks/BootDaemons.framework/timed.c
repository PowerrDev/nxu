/*
 * timed: the wall clock and the time zone.
 *
 * The RTC keeps UTC (nxu_time). timed adds the chosen zone: a fixed table of
 * tz-database names, each a standard offset and one of a few daylight-saving
 * rules, so local time is right on either side of a change without a zone
 * database on disk. The choice lives in com.nxu.timed (TimeZone) together
 * with the offset in effect (OffsetSeconds), which timed rewrites when
 * daylight saving starts or ends: the menu bar reads only that.
 */
#include <nxu/services.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#define TIMED_PREFERENCES "/Volumes/mainOS/Library/Preferences/com.nxu.timed.plist"
#define DEFAULT_ZONE 9U /* UTC */

typedef enum { RULE_NONE, RULE_US, RULE_EU, RULE_AU, RULE_NZ } rule_t;

typedef struct {
	const char *name, *city, *standard, *daylight;
	int32_t offset_minutes;
	rule_t rule;
} zone_t;

static const zone_t g_zones[] = {
	{"Pacific/Honolulu", "Honolulu", "HST", "HST", -600, RULE_NONE},
	{"America/Anchorage", "Anchorage", "AKST", "AKDT", -540, RULE_US},
	{"America/Los_Angeles", "Cupertino", "PST", "PDT", -480, RULE_US},
	{"America/Denver", "Denver", "MST", "MDT", -420, RULE_US},
	{"America/Phoenix", "Phoenix", "MST", "MST", -420, RULE_NONE},
	{"America/Chicago", "Chicago", "CST", "CDT", -360, RULE_US},
	{"America/New_York", "New York", "EST", "EDT", -300, RULE_US},
	{"America/Sao_Paulo", "Sao Paulo", "-03", "-03", -180, RULE_NONE},
	{"Atlantic/Azores", "Azores", "-01", "+00", -60, RULE_EU},
	{"UTC", "Coordinated Universal Time", "UTC", "UTC", 0, RULE_NONE},
	{"Europe/London", "London", "GMT", "BST", 0, RULE_EU},
	{"Europe/Lisbon", "Lisbon", "WET", "WEST", 0, RULE_EU},
	{"Europe/Paris", "Paris", "CET", "CEST", 60, RULE_EU},
	{"Europe/Berlin", "Berlin", "CET", "CEST", 60, RULE_EU},
	{"Europe/Madrid", "Madrid", "CET", "CEST", 60, RULE_EU},
	{"Europe/Athens", "Athens", "EET", "EEST", 120, RULE_EU},
	{"Africa/Cairo", "Cairo", "EET", "EET", 120, RULE_NONE},
	{"Europe/Moscow", "Moscow", "MSK", "MSK", 180, RULE_NONE},
	{"Asia/Dubai", "Dubai", "+04", "+04", 240, RULE_NONE},
	{"Asia/Kolkata", "Kolkata", "IST", "IST", 330, RULE_NONE},
	{"Asia/Bangkok", "Bangkok", "+07", "+07", 420, RULE_NONE},
	{"Asia/Shanghai", "Shanghai", "CST", "CST", 480, RULE_NONE},
	{"Asia/Singapore", "Singapore", "+08", "+08", 480, RULE_NONE},
	{"Asia/Tokyo", "Tokyo", "JST", "JST", 540, RULE_NONE},
	{"Australia/Sydney", "Sydney", "AEST", "AEDT", 600, RULE_AU},
	{"Pacific/Auckland", "Auckland", "NZST", "NZDT", 720, RULE_NZ},
};

#define ZONE_COUNT (sizeof(g_zones) / sizeof(g_zones[0]))

static uint32_t g_zone = DEFAULT_ZONE;
static int32_t g_published = INT32_MIN;
static int32_t g_logged = INT32_MIN;
/* The last publish bypassed preferencesd (it was not running yet). */
static bool g_published_directly;

static void
log_line(const char *first, const char *second)
{
	(void)nxu_write(1, "timed: ", 7);
	(void)nxu_write(1, first, nxu_strlen(first));
	if (second) (void)nxu_write(1, second, nxu_strlen(second));
	(void)nxu_write(1, "\n", 1);
}

/* Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's). */
static int64_t
days_from_civil(int64_t year, uint32_t month, uint32_t day)
{
	year -= month <= 2U;
	int64_t era = (year >= 0 ? year : year - 399) / 400;
	uint64_t yoe = (uint64_t)(year - era * 400);
	uint64_t doy = (153U * (month > 2U ? month - 3U : month + 9U) + 2U) / 5U + day - 1U;
	uint64_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

static int64_t
year_of(int64_t seconds)
{
	int64_t days = seconds / 86400 - (seconds % 86400 < 0);
	int64_t z = days + 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	uint64_t doe = (uint64_t)(z - era * 146097);
	uint64_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
	uint64_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
	uint64_t mp = (5U * doy + 2U) / 153U;
	return (int64_t)yoe + era * 400 + (mp >= 10U);
}

/* 0 = Sunday. */
static uint32_t
weekday(int64_t days)
{
	int64_t w = (days + 4) % 7;
	return (uint32_t)(w < 0 ? w + 7 : w);
}

/* The day of the `n`th Sunday of a month; n = 0 for the last. */
static int64_t
sunday(int64_t year, uint32_t month, uint32_t n)
{
	if (n == 0) {
		int64_t next = month == 12U ? days_from_civil(year + 1, 1, 1) : days_from_civil(year, month + 1U, 1);
		int64_t last = next - 1;
		return last - weekday(last);
	}
	int64_t first = days_from_civil(year, month, 1);
	return first + (7 - weekday(first)) % 7 + 7 * (int64_t)(n - 1U);
}

/* Whether daylight saving is in effect at `utc` in `zone`. */
static bool
daylight(const zone_t *zone, int64_t utc)
{
	int64_t standard = (int64_t)zone->offset_minutes * 60;
	int64_t year = year_of(utc + standard);
	int64_t start, end;

	switch (zone->rule) {
	case RULE_US: /* 2nd Sunday in March 02:00 to 1st Sunday in November 02:00, local */
		start = sunday(year, 3, 2) * 86400 + 7200 - standard;
		end = sunday(year, 11, 1) * 86400 + 7200 - standard - 3600;
		return utc >= start && utc < end;
	case RULE_EU: /* last Sunday in March to last Sunday in October, 01:00 UTC */
		start = sunday(year, 3, 0) * 86400 + 3600;
		end = sunday(year, 10, 0) * 86400 + 3600;
		return utc >= start && utc < end;
	case RULE_AU: /* 1st Sunday in October 02:00 to 1st Sunday in April 03:00 (DST), local */
		start = sunday(year, 10, 1) * 86400 + 7200 - standard;
		end = sunday(year, 4, 1) * 86400 + 10800 - standard - 3600;
		return utc >= start || utc < end;
	case RULE_NZ: /* last Sunday in September 02:00 to 1st Sunday in April 03:00 (DST), local */
		start = sunday(year, 9, 0) * 86400 + 7200 - standard;
		end = sunday(year, 4, 1) * 86400 + 10800 - standard - 3600;
		return utc >= start || utc < end;
	default:
		return false;
	}
}

static void
now(nxu_time_info_t *info)
{
	const zone_t *zone = &g_zones[g_zone];
	int64_t utc = nxu_time();
	bool dst = daylight(zone, utc);

	memset(info, 0, sizeof(*info));
	info->utc_seconds = utc;
	info->offset_seconds = zone->offset_minutes * 60 + (dst ? 3600 : 0);
	info->local_seconds = utc + info->offset_seconds;
	info->daylight = dst;
	nxu_service_copy(info->zone, sizeof(info->zone), zone->name);
	nxu_service_copy(info->abbreviation, sizeof(info->abbreviation), dst ? zone->daylight : zone->standard);
}

/* Write com.nxu.timed: the zone and the offset now in effect. */
static void
publish(void)
{
	nxu_time_info_t info;

	now(&info);
	if (info.offset_seconds == g_published) return;

	char text[512];
	char number[16];
	int32_t offset = info.offset_seconds;
	uint32_t magnitude = (uint32_t)(offset < 0 ? -offset : offset), length = 0, at = 0;
	char reversed[16];

	do {
		reversed[length++] = (char)('0' + magnitude % 10U);
		magnitude /= 10U;
	} while (magnitude);
	if (offset < 0) number[at++] = '-';
	while (length) number[at++] = reversed[--length];
	number[at] = '\0';

	const char *parts[] = {
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n\t<key>TimeZone</key>\n\t<string>",
		info.zone,
		"</string>\n\t<key>OffsetSeconds</key>\n\t<integer>",
		number,
		"</integer>\n</dict>\n</plist>\n",
	};
	uint64_t used = 0;

	for (uint32_t index = 0; index < sizeof(parts) / sizeof(parts[0]); index++) {
		uint64_t part = nxu_strlen(parts[index]);

		if (used + part >= sizeof(text)) return;
		memcpy(text + used, parts[index], part);
		used += part;
	}

	/* Through preferencesd, so what watches com.nxu.timed (the menu bar
	 * clock) hears of it; written directly only while there is none yet. */
	if (nxu_preferences_write("com.nxu.timed", text, used) != 0) {
		g_published_directly = true;
		int64_t fd = nxu_open(TIMED_PREFERENCES, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);

		if (fd < 0) {
			log_line("cannot write ", TIMED_PREFERENCES);
			return;
		}
		(void)nxu_write((uint64_t)fd, text, used);
		(void)nxu_close((uint64_t)fd);
		(void)nxu_sync();
	} else {
		g_published_directly = false;
	}
	if (info.offset_seconds != g_logged) log_line("local time is ", info.abbreviation);
	g_logged = info.offset_seconds;
	g_published = info.offset_seconds;
}

static int32_t
zone_index(const char *name)
{
	for (uint32_t index = 0; index < ZONE_COUNT; index++)
		if (nxu_streq(g_zones[index].name, name)) return (int32_t)index;
	return -1;
}

/* The zone chosen before, from com.nxu.timed. */
static void
load(void)
{
	static char text[1024];
	int64_t fd = nxu_open(TIMED_PREFERENCES, NXU_O_READ);

	if (fd < 0) return;

	int64_t length = nxu_read((uint64_t)fd, text, sizeof(text) - 1U);

	(void)nxu_close((uint64_t)fd);
	if (length <= 0) return;
	text[length] = '\0';

	const char *key = "<key>TimeZone</key>";
	uint64_t key_length = nxu_strlen(key);

	for (int64_t at = 0; at + (int64_t)key_length < length; at++) {
		uint64_t same = 0;

		while (same < key_length && text[at + (int64_t)same] == key[same]) same++;
		if (same != key_length) continue;

		char *start = text + at + key_length;
		while (*start && *start != '>') start++;
		if (!*start) return;
		start++;
		char *end = start;
		while (*end && *end != '<') end++;
		*end = '\0';

		int32_t index = zone_index(start);
		if (index >= 0) g_zone = (uint32_t)index;
		return;
	}
}

static void
handle(nxu_service_message_t *message, const nxu_ipc_peer_t *peer)
{
	message->length = 0;
	switch (message->operation) {
	case NXU_TIMED_NOW: {
		nxu_time_info_t info;

		now(&info);
		memcpy(message->data, &info, sizeof(info));
		message->length = sizeof(info);
		break;
	}
	case NXU_TIMED_ZONE: {
		if (message->arguments[0] >= ZONE_COUNT) {
			message->status = (int32_t)-NXU_SYS_E_NOT_FOUND;
			break;
		}
		const zone_t *zone = &g_zones[message->arguments[0]];
		nxu_time_zone_t out = {0};

		out.standard_offset_seconds = zone->offset_minutes * 60;
		out.has_daylight = zone->rule != RULE_NONE;
		nxu_service_copy(out.name, sizeof(out.name), zone->name);
		nxu_service_copy(out.city, sizeof(out.city), zone->city);
		memcpy(message->data, &out, sizeof(out));
		message->length = sizeof(out);
		break;
	}
	case NXU_TIMED_SET_ZONE: {
		int32_t index = zone_index(message->name);

		if (peer->uid != 0U) {
			message->status = (int32_t)-NXU_SYS_E_DENIED;
		} else if (index < 0) {
			message->status = (int32_t)-NXU_SYS_E_NOT_FOUND;
		} else {
			g_zone = (uint32_t)index;
			g_published = INT32_MIN;
			log_line("time zone set to ", g_zones[index].name);
			publish();
		}
		break;
	}
	default:
		message->status = (int32_t)-NXU_SYS_E_NOT_SUPPORTED;
		break;
	}
}

int
main(void)
{
	(void)nxu_mkdir("/Volumes/mainOS/Library/Preferences");
	load();
	publish();
	log_line("ready in ", g_zones[g_zone].name);
	/* preferencesd may start after timed: say it again through it once it
	 * answers, so watchers are told. */
	for (uint32_t attempt = 0; g_published_directly && attempt < 40U; attempt++) {
		(void)nxu_sleep_us(250000U);
		g_published = INT32_MIN;
		publish();
	}
	/* Once a minute is enough to catch a daylight-saving change. */
	return nxu_service_run(NXU_TIMED_SERVICE, handle, publish, 60000000U);
}
