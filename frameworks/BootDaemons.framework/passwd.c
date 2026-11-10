/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * passwd -- set an account's password. A user may change their own password
 * after confirming the current one; root may change any account without it. The
 * new password is salted and hashed into /etc/shadow.
 *
 * Writing /etc/shadow (0600, root-owned) needs privilege, so passwd is installed
 * setuid-root (chmod 4755).
 */
#include <nxu/syscall.h>
#include <nxu/pwdb.h>
#include <nxu/auth.h>
#include <nxu/string.h>

#include <stdint.h>

static void
say(const char *text)
{
	uint64_t length = 0ULL;
	while (text[length] != '\0')
		length++;
	(void)nxu_write(1ULL, text, length);
}

static int64_t
read_line(char *buffer, uint32_t capacity)
{
	uint32_t length = 0U;
	while (length + 1U < capacity) {
		char c;
		int64_t count = nxu_read(0ULL, &c, 1ULL);
		if (count <= 0) {
			if (length == 0U) return -1;
			break;
		}
		if (c == '\n' || c == '\r') break;
		buffer[length++] = c;
	}
	buffer[length] = '\0';
	return (int64_t)length;
}

int
main(uint64_t argc, const char *const *argv)
{
	nxu_passwd_t caller;
	if (nxu_getpwuid((uint32_t)nxu_getuid(), &caller) != 0) {
		say("passwd: cannot identify the calling user\n");
		return 1;
	}

	const char *target = (argc >= 2U) ? argv[1] : caller.pw_name;

	nxu_passwd_t tpw;
	if (nxu_getpwnam(target, &tpw) != 0) {
		say("passwd: unknown user: ");
		say(target);
		say("\n");
		return 1;
	}

	if (nxu_getuid() != 0) {
		if (!nxu_streq(target, caller.pw_name)) {
			say("passwd: you may only change your own password\n");
			return 1;
		}
		say("Current password: ");
		char current[128];
		int64_t length = read_line(current, sizeof(current));
		say("\n");
		if (length < 0 || !nxu_authenticate(caller.pw_name, current)) {
			say("passwd: authentication failure\n");
			return 1;
		}
	}

	say("New password: ");
	char first[128];
	int64_t first_len = read_line(first, sizeof(first));
	say("\n");
	say("Retype new password: ");
	char second[128];
	(void)read_line(second, sizeof(second));
	say("\n");

	if (first_len <= 0) {
		say("passwd: an empty password is not allowed\n");
		return 1;
	}
	if (!nxu_streq(first, second)) {
		say("passwd: passwords do not match\n");
		return 1;
	}

	uint8_t salt[NXU_PW_SALT_LEN];
	(void)nxu_pwsalt(salt);

	char crypt[NXU_CRYPT_MAX];
	if (nxu_pwhash_make(first, salt, NXU_PW_ITERATIONS, crypt, sizeof(crypt)) != 0) {
		say("passwd: could not hash the password\n");
		return 1;
	}
	if (nxu_shadow_set(target, crypt) != 0) {
		say("passwd: could not update /etc/shadow\n");
		return 1;
	}

	say("passwd: password changed for ");
	say(target);
	say("\n");
	return 0;
}
