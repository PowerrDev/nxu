/*
 * defaults -- read and change preferences through preferencesd, after
 * macOS's defaults(1):
 *
 *   defaults read DOMAIN
 *   defaults write DOMAIN KEY -string VALUE | -int VALUE | -bool true|false
 *   defaults delete DOMAIN [KEY]
 */
#include <nxu/services.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

static void
say(const char *text)
{
	(void)nxu_write(1, text, nxu_strlen(text));
}

static int
fail(const char *what, int64_t status)
{
	char digits[4] = {0};
	uint64_t code = (uint64_t)(status < 0 ? -status : status);

	digits[0] = (char)('0' + (code / 10U) % 10U);
	digits[1] = (char)('0' + code % 10U);
	say("defaults: ");
	say(what);
	say(status == -NXU_SYS_E_DENIED	   ? ": permission denied (run it with sudo)\n"
		: status == -NXU_SYS_E_NOT_FOUND ? ": not found\n"
										 : ": failed (error -");
	if (status != -NXU_SYS_E_DENIED && status != -NXU_SYS_E_NOT_FOUND) {
		say(digits);
		say(")\n");
	}
	return 1;
}

static int
usage(void)
{
	say("usage: defaults read DOMAIN\n"
		"       defaults write DOMAIN KEY -string VALUE | -int VALUE | -bool true|false\n"
		"       defaults delete DOMAIN [KEY]\n");
	return 2;
}

int
main(uint64_t argc, const char *const *argv)
{
	if (argc < 3) return usage();

	const char *verb = argv[1], *domain = argv[2];

	if (nxu_streq(verb, "read") && argc == 3) {
		static char text[NXU_SERVICE_DATA_MAX + 1U];
		int64_t status = nxu_preferences_read(domain, text, sizeof(text));

		if (status < 0) return fail(domain, status);
		say(text);
		return 0;
	}

	if (nxu_streq(verb, "write") && argc == 6) {
		const char *key = argv[3], *type = argv[4], *value = argv[5];
		int64_t status;

		if (nxu_streq(type, "-string")) {
			status = nxu_preferences_set_string(domain, key, value);
		} else if (nxu_streq(type, "-int") || nxu_streq(type, "-integer")) {
			int64_t number = 0;
			bool negative = *value == '-';

			for (const char *digit = value + negative; *digit; digit++) {
				if (*digit < '0' || *digit > '9') return usage();
				number = number * 10 + (*digit - '0');
			}
			status = nxu_preferences_set_integer(domain, key, negative ? -number : number);
		} else if (nxu_streq(type, "-bool") || nxu_streq(type, "-boolean")) {
			bool yes = nxu_streq(value, "true") || nxu_streq(value, "yes") || nxu_streq(value, "1");

			if (!yes && !nxu_streq(value, "false") && !nxu_streq(value, "no") && !nxu_streq(value, "0"))
				return usage();
			status = nxu_preferences_set_boolean(domain, key, yes);
		} else {
			return usage();
		}
		return status < 0 ? fail(domain, status) : 0;
	}

	if (nxu_streq(verb, "delete") && (argc == 3 || argc == 4)) {
		int64_t status = nxu_preferences_remove(domain, argc == 4 ? argv[3] : "");

		return status < 0 ? fail(domain, status) : 0;
	}
	return usage();
}
