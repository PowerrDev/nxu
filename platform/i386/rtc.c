/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/i386/rtc.c
 *
 * The PC's CMOS real-time clock behind <platform/rtc.h>.
 *
 * The clock keeps calendar fields in CMOS RAM, in BCD or binary and in 12 or
 * 24 hour form depending on status register B, and the fields change once a
 * second while an update-in-progress flag is raised in status register A. So a
 * read waits for the flag to drop and repeats until two successive snapshots
 * agree, then converts the snapshot to seconds since the Unix epoch, taking
 * the clock to be UTC as QEMU's default does.
 */

#include <platform/rtc.h>

#include <kern/i386/io.h>

#include <stdbool.h>
#include <stdint.h>

#define CMOS_INDEX_PORT 0x70U
#define CMOS_DATA_PORT 0x71U

#define CMOS_SECONDS 0x00U
#define CMOS_MINUTES 0x02U
#define CMOS_HOURS 0x04U
#define CMOS_DAY 0x07U
#define CMOS_MONTH 0x08U
#define CMOS_YEAR 0x09U
#define CMOS_STATUS_A 0x0AU
#define CMOS_STATUS_B 0x0BU
#define CMOS_STATUS_D 0x0DU
#define CMOS_CENTURY 0x32U

#define CMOS_STATUS_A_UPDATE_IN_PROGRESS 0x80U
#define CMOS_STATUS_B_24_HOUR 0x02U
#define CMOS_STATUS_B_BINARY 0x04U
#define CMOS_HOUR_PM 0x80U

#define RTC_UIP_SPINS 100000U
#define RTC_SNAPSHOT_TRIES 8U

typedef struct {
	uint8_t second;
	uint8_t minute;
	uint8_t hour;
	uint8_t day;
	uint8_t month;
	uint8_t year;
	uint8_t century;
} rtc_snapshot_t;

static bool g_rtc_ready;

static uint8_t rtc_cmos_read(uint8_t reg)
{
	/* Bit 7 of the index port is the NMI mask; leave NMIs enabled. */
	outb(CMOS_INDEX_PORT, reg);
	return inb(CMOS_DATA_PORT);
}

static uint8_t rtc_bcd_to_binary(uint8_t value)
{
	return (uint8_t)((value & 0x0FU) + (value >> 4U) * 10U);
}

/* Days from 1970-01-01 to a proleptic Gregorian civil date (Hinnant). */
static uint64_t rtc_days_from_civil(uint32_t year, uint32_t month, uint32_t day)
{
	uint32_t shifted_year = month <= 2U ? year - 1U : year;
	uint32_t era = shifted_year / 400U;
	uint32_t year_of_era = shifted_year - era * 400U;
	uint32_t day_of_year = (153U * (month > 2U ? month - 3U : month + 9U) + 2U) / 5U + day - 1U;
	uint32_t day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;

	return (uint64_t)era * 146097ULL + day_of_era - 719468ULL;
}

static void rtc_read_raw(rtc_snapshot_t *snapshot)
{
	for (uint32_t spin = 0U; spin < RTC_UIP_SPINS; spin++) {
		if ((rtc_cmos_read(CMOS_STATUS_A) & CMOS_STATUS_A_UPDATE_IN_PROGRESS) == 0U) break;
	}

	snapshot->second = rtc_cmos_read(CMOS_SECONDS);
	snapshot->minute = rtc_cmos_read(CMOS_MINUTES);
	snapshot->hour = rtc_cmos_read(CMOS_HOURS);
	snapshot->day = rtc_cmos_read(CMOS_DAY);
	snapshot->month = rtc_cmos_read(CMOS_MONTH);
	snapshot->year = rtc_cmos_read(CMOS_YEAR);
	snapshot->century = rtc_cmos_read(CMOS_CENTURY);
}

static bool rtc_snapshot_equal(const rtc_snapshot_t *left, const rtc_snapshot_t *right)
{
	return left->second == right->second && left->minute == right->minute &&
		left->hour == right->hour && left->day == right->day &&
		left->month == right->month && left->year == right->year &&
		left->century == right->century;
}

/*
 * rtc_read_seconds:
 *
 * One consistent reading of the clock, or 0 if the fields do not describe a
 * date (an unset or corrupted CMOS).
 */
static uint64_t rtc_read_seconds(void)
{
	rtc_snapshot_t previous;
	rtc_snapshot_t current;

	rtc_read_raw(&previous);

	for (uint32_t attempt = 0U; attempt < RTC_SNAPSHOT_TRIES; attempt++) {
		rtc_read_raw(&current);

		if (rtc_snapshot_equal(&previous, &current)) break;

		previous = current;
	}

	uint8_t status_b = rtc_cmos_read(CMOS_STATUS_B);
	bool binary = (status_b & CMOS_STATUS_B_BINARY) != 0U;
	bool twenty_four = (status_b & CMOS_STATUS_B_24_HOUR) != 0U;
	bool pm = !twenty_four && (current.hour & CMOS_HOUR_PM) != 0U;

	/* The PM flag is the top bit of the hour register in 12-hour mode. */
	uint8_t hour_raw = twenty_four ? current.hour : (uint8_t)(current.hour & ~CMOS_HOUR_PM);

	uint32_t second = binary ? current.second : rtc_bcd_to_binary(current.second);
	uint32_t minute = binary ? current.minute : rtc_bcd_to_binary(current.minute);
	uint32_t hour = binary ? hour_raw : rtc_bcd_to_binary(hour_raw);
	uint32_t day = binary ? current.day : rtc_bcd_to_binary(current.day);
	uint32_t month = binary ? current.month : rtc_bcd_to_binary(current.month);
	uint32_t year = binary ? current.year : rtc_bcd_to_binary(current.year);
	uint32_t century = binary ? current.century : rtc_bcd_to_binary(current.century);

	if (!twenty_four) {
		hour %= 12U;
		if (pm) hour += 12U;
	}

	/* Century register: absent or unset on some machines; assume 20xx then. */
	if (century < 19U || century > 21U) century = 20U;

	year += century * 100U;

	if (second > 59U || minute > 59U || hour > 23U || day < 1U || day > 31U || month < 1U || month > 12U) return 0ULL;

	uint64_t days = rtc_days_from_civil(year, month, day);

	return days * 86400ULL + (uint64_t)hour * 3600ULL + (uint64_t)minute * 60ULL + second;
}

bool rtc_init(void)
{
	if (g_rtc_ready) return true;

	/* A CMOS that does not hold a valid date (or is not there) reads as zero. */
	if (rtc_read_seconds() == 0ULL) return false;

	g_rtc_ready = true;
	return true;
}

uint64_t rtc_unix_time(void)
{
	if (!g_rtc_ready) return 0ULL;

	return rtc_read_seconds();
}
