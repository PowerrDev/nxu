/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * nxu/utmp.h
 *
 * Who is logged in, and who was: the BSD utmp and wtmp files, with records
 * of one fixed layout.
 *
 *   utmp  /var/run/utmp   one record per terminal, the session on it now: a
 *                         USER_PROCESS record while someone is logged in, a
 *                         DEAD_PROCESS one once they have left (who)
 *   wtmp  /var/log/wtmp   every login and logout, appended in order (last)
 *
 * login writes the login records while it is still root; getty, which
 * outlives the session, writes the logout records after it. Times are UTC
 * seconds (nxu_time); nxu_utmp_format_time shows them in local time.
 * Both files are root's, mode 0644.
 */
#ifndef NXU_UTMP_H
#define NXU_UTMP_H

#include <stdbool.h>
#include <stdint.h>

#define NXU_UTMP_PATH "/Volumes/mainOS/var/run/utmp"
#define NXU_WTMP_PATH "/Volumes/mainOS/var/log/wtmp"

#define NXU_UT_NAMESIZE 32U
#define NXU_UT_LINESIZE 32U
#define NXU_UT_HOSTSIZE 64U

/* Record types, numbered as utmpx's. */
#define NXU_UT_BOOT_TIME 2U
#define NXU_UT_USER_PROCESS 7U
#define NXU_UT_DEAD_PROCESS 8U

/* The most terminals utmp keeps a record for. */
#define NXU_UTMP_MAX 32U

/*
 * nxu_utmp_t
 *
 * ut_line is the terminal's name in /dev ("console"), ut_user the account
 * (empty in a DEAD_PROCESS record of utmp), ut_host where the login came
 * from (empty: the machine's own terminal), ut_pid the login process.
 */
typedef struct {
	uint32_t ut_type;
	uint32_t ut_pid;
	char ut_user[NXU_UT_NAMESIZE];
	char ut_line[NXU_UT_LINESIZE];
	char ut_host[NXU_UT_HOSTSIZE];
	int64_t ut_time;
} nxu_utmp_t;

_Static_assert(sizeof(nxu_utmp_t) == 144U, "nxu_utmp_t is the file format: its layout must not change");

/*
 * nxu_utmp_login / nxu_utmp_logout
 *
 * A session began on `line` (a terminal's path or its name in /dev), for
 * `user`, by login process `pid`; and it ended. Each sets the terminal's
 * utmp record and appends to wtmp. 0, or -1 if a file could not be written.
 * Root only (the files are root's).
 */
int nxu_utmp_login(const char *line, const char *user, uint32_t pid, int64_t time);
int nxu_utmp_logout(const char *line, int64_t time);

/*
 * nxu_utmp_clear
 *
 * Forget a session utmp still has on `line` without recording a logout: for
 * getty starting on a terminal, whose last session ended with the machine
 * (last then shows it as "gone - no logout"). True when there was one.
 */
bool nxu_utmp_clear(const char *line);

/* Whether utmp has someone logged in on `line`. */
bool nxu_utmp_active(const char *line);

/*
 * nxu_utmp_read
 *
 * The last `capacity` records of the file at `path`, oldest first, into
 * `records`; returns how many (0 for a file that does not exist), or -1.
 * *total, if not null, is how many the file holds.
 */
int64_t nxu_utmp_read(const char *path, nxu_utmp_t *records, uint32_t capacity, uint32_t *total);

/* The latest login of `user` in wtmp; false if there is none. */
bool nxu_utmp_last_login(const char *user, nxu_utmp_t *record);

/*
 * nxu_utmp_local_offset / nxu_utmp_format_time
 *
 * How far local time is from UTC now, in seconds (from timed; 0 on a
 * system that does not run it), and `utc` + `offset` as "Sat Nov 14 14:20" (16 characters and the
 * terminator), or as "14:20" (5 and the terminator) when `short_form`.
 */
int64_t nxu_utmp_local_offset(void);
void nxu_utmp_format_time(int64_t utc, int64_t offset, bool short_form, char *out);

#endif
