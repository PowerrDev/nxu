#ifndef NXU_RTC_H
#define NXU_RTC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Map the PL031 real-time clock so wall-clock time is available to
 * userspace-facing services such as UIService's menu bar. Safe to call more
 * than once; only the first call does any work.
 */
bool rtc_init(void);

/* Seconds since 1970-01-01 00:00:00 UTC, or 0 if rtc_init() has not
 * succeeded yet. */
uint64_t rtc_unix_time(void);

#endif
