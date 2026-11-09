/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/security/ucred.h
 *
 * Process credentials: the Unix user/group identity the kernel owns and
 * enforces. Modeled on XNU's bsd/sys/ucred.h (see doc/architecture/
 * unix-security.md). Phase 1 of the native Unix security substrate.
 *
 * A credential carries real, effective and saved user and group IDs plus a
 * supplementary group list. The effective IDs decide what the process may do;
 * the real and saved IDs bound which effective IDs it may switch to without
 * privilege. Effective UID 0 is root: unrestricted, and bridged to the full
 * capability set (NXU_CAP_ALL) so existing capability checks keep working.
 *
 * Unlike XNU's reference-counted, shared kauth_cred_t, a ucred is embedded in
 * struct proc with copy semantics. Simpler for a first cut; revisit if sharing
 * cost matters.
 */
#ifndef NXU_KERN_UCRED_H
#define NXU_KERN_UCRED_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t nxu_uid_t;
typedef uint32_t nxu_gid_t;

/* Well-known identities. */
#define NXU_UID_ROOT 0U
#define NXU_GID_WHEEL 0U
#define NXU_UID_NOBODY 65534U
#define NXU_GID_NOBODY 65534U

/* NXU_ID_UNCHANGED ("no change" for a uid/gid in chown) is defined in
 * kern/syscall/syscall_defs.h so userland shares it. */

/* Supplementary groups carried per process. */
#define NXU_NGROUPS_MAX 16U

/*
 * struct ucred
 *
 * cr_ruid / cr_uid / cr_svuid   real, effective, saved user ID
 * cr_rgid / cr_gid / cr_svgid   real, effective, saved group ID
 * cr_groups[0 .. cr_ngroups-1]  supplementary groups (cr_ngroups <= NXU_NGROUPS_MAX)
 */
struct ucred {
	nxu_uid_t cr_ruid;
	nxu_uid_t cr_uid;
	nxu_uid_t cr_svuid;
	nxu_gid_t cr_rgid;
	nxu_gid_t cr_gid;
	nxu_gid_t cr_svgid;
	uint32_t cr_ngroups;
	nxu_gid_t cr_groups[NXU_NGROUPS_MAX];
};

/* Initialize cred to root (uid/gid 0, no supplementary groups). */
static inline void
ucred_init_root(struct ucred *cred)
{
	if (cred == 0) return;
	cred->cr_ruid = cred->cr_uid = cred->cr_svuid = NXU_UID_ROOT;
	cred->cr_rgid = cred->cr_gid = cred->cr_svgid = NXU_GID_WHEEL;
	cred->cr_ngroups = 0U;
	for (uint32_t i = 0U; i < NXU_NGROUPS_MAX; i++) cred->cr_groups[i] = 0U;
}

/* Copy src into dst (inheritance at fork/spawn/create). */
static inline void
ucred_copy(struct ucred *dst, const struct ucred *src)
{
	if (dst == 0 || src == 0) return;
	*dst = *src;
}

/* True when the credential is privileged (effective UID is root). */
static inline bool
ucred_is_root(const struct ucred *cred)
{
	return cred != 0 && cred->cr_uid == NXU_UID_ROOT;
}

/* True when gid is the credential's effective or real group, or one of its
 * supplementary groups. */
static inline bool
ucred_in_group(const struct ucred *cred, nxu_gid_t gid)
{
	if (cred == 0) return false;
	if (cred->cr_gid == gid || cred->cr_rgid == gid) return true;
	for (uint32_t i = 0U; i < cred->cr_ngroups; i++)
		if (cred->cr_groups[i] == gid) return true;
	return false;
}

/*
 * May this credential adopt target as an effective UID without privilege?
 * POSIX: a non-root process may set its effective UID only to its real or
 * saved UID. Root may set any.
 */
static inline bool
ucred_may_seteuid(const struct ucred *cred, nxu_uid_t target)
{
	if (cred == 0) return false;
	return ucred_is_root(cred) || target == cred->cr_ruid || target == cred->cr_svuid;
}

static inline bool
ucred_may_setegid(const struct ucred *cred, nxu_gid_t target)
{
	if (cred == 0) return false;
	return ucred_is_root(cred) || target == cred->cr_rgid || target == cred->cr_svgid;
}

/*
 * setuid(uid). Root sets real, effective and saved all to uid. A non-root
 * process may set its effective UID to its real or saved UID (and nothing
 * else). Returns false if the transition is not permitted.
 */
static inline bool
ucred_setuid(struct ucred *cred, nxu_uid_t uid)
{
	if (cred == 0) return false;
	if (ucred_is_root(cred)) {
		cred->cr_ruid = cred->cr_uid = cred->cr_svuid = uid;
		return true;
	}
	if (uid == cred->cr_ruid || uid == cred->cr_svuid) {
		cred->cr_uid = uid;
		return true;
	}
	return false;
}

static inline bool
ucred_setgid(struct ucred *cred, nxu_gid_t gid)
{
	if (cred == 0) return false;
	if (ucred_is_root(cred)) {
		cred->cr_rgid = cred->cr_gid = cred->cr_svgid = gid;
		return true;
	}
	if (gid == cred->cr_rgid || gid == cred->cr_svgid) {
		cred->cr_gid = gid;
		return true;
	}
	return false;
}

/* seteuid(uid): change only the effective UID, within the rule above. */
static inline bool
ucred_seteuid(struct ucred *cred, nxu_uid_t uid)
{
	if (!ucred_may_seteuid(cred, uid)) return false;
	cred->cr_uid = uid;
	return true;
}

static inline bool
ucred_setegid(struct ucred *cred, nxu_gid_t gid)
{
	if (!ucred_may_setegid(cred, gid)) return false;
	cred->cr_gid = gid;
	return true;
}

#endif
