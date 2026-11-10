/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * nxu/pwdb.h
 *
 * Reading the account database: /etc/passwd and /etc/group. A small userland
 * replacement for getpwnam/getpwuid/getgrgid that parses the colon-separated
 * files directly. Lookups return 0 on success and -1 when the entry is missing
 * or the file cannot be read.
 */
#ifndef NXU_PWDB_H
#define NXU_PWDB_H

#include <stdint.h>

#define NXU_PASSWD_PATH "/Volumes/mainOS/etc/passwd"
#define NXU_GROUP_PATH "/Volumes/mainOS/etc/group"

#define NXU_PW_NAME_MAX 33U
#define NXU_PW_PATH_MAX 128U

typedef struct {
	char pw_name[NXU_PW_NAME_MAX];
	uint32_t pw_uid;
	uint32_t pw_gid;
	char pw_dir[NXU_PW_PATH_MAX];
	char pw_shell[NXU_PW_PATH_MAX];
} nxu_passwd_t;

typedef struct {
	char gr_name[NXU_PW_NAME_MAX];
	uint32_t gr_gid;
} nxu_group_t;

int nxu_getpwuid(uint32_t uid, nxu_passwd_t *out);
int nxu_getpwnam(const char *name, nxu_passwd_t *out);
int nxu_getgrgid(uint32_t gid, nxu_group_t *out);

#endif
