#include <frameworks/SoftwareUpdate.framework/include/nsw.h>

#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

#define PATCHD_BUFFER_SIZE 16384U
/*
 * How often the system images are compared with their recovery copies. Each
 * check reads every image twice through the boot CPU (where file system calls
 * run, next to the desktop): every 5 s it was a stutter the user could feel.
 */
#define PATCHD_INTERVAL_US 30000000ULL
/* How often the loop looks for a repair request: an open() per pass of a yield loop is real work. */
#define PATCHD_REQUEST_POLL_US 500000ULL
#define PATCHD_REQUEST_PATH "/disk/var/db/patchd/repair-all"

typedef struct {
	const char *primary;
	const char *recovery;
} patchd_target_t;

static const patchd_target_t g_targets[] = {
	{"/disk/System/Library/CoreServices/bootd", "/disk/System/Library/CoreServices/bootd.recovery"},
	{"/disk/System/Library/CoreServices/logd", "/disk/System/Library/CoreServices/logd.recovery"},
	{"/disk/System/Library/CoreServices/patchd", "/disk/System/Library/CoreServices/patchd.recovery"}};

static void
patchd_log(const char *message)
{
	(void)nxu_write(1U, message, nxu_strlen(message));
}

static bool
patchd_images_equal(const patchd_target_t *target)
{
	int64_t primary = nxu_open(target->primary, NXU_O_READ);
	if (primary < 0) {
		return false;
	}
	int64_t recovery = nxu_open(target->recovery, NXU_O_READ);
	if (recovery < 0) {
		(void)nxu_close((uint64_t)primary);
		return false;
	}

	/* Static: 16 KiB chunks mean few system calls, and i386 stacks are 16 KiB. */
	static char primary_buffer[PATCHD_BUFFER_SIZE];
	static char recovery_buffer[PATCHD_BUFFER_SIZE];
	bool equal = true;

	for (;;) {
		int64_t primary_read = nxu_read((uint64_t)primary, primary_buffer, sizeof(primary_buffer));
		int64_t recovery_read = nxu_read((uint64_t)recovery, recovery_buffer, sizeof(recovery_buffer));
		if (primary_read < 0 || recovery_read < 0 || primary_read != recovery_read) {
			equal = false;
			break;
		}

		if (primary_read == 0) {
			break;
		}

		for (int64_t index = 0; index < primary_read; index++) {
			if (primary_buffer[index] == recovery_buffer[index]) {
				continue;
			}
			equal = false;
			break;
		}

		if (!equal) {
			break;
		}

		/*
		 * The check reads whole system images. Done in one go it holds the CPU
		 * for tens of milliseconds every interval and the graphical session
		 * (which shares the CPU by yielding) stalls behind it, so give the
		 * others a turn after every chunk.
		 */
		(void)nxu_yield();
	}

	(void)nxu_close((uint64_t)recovery);
	(void)nxu_close((uint64_t)primary);
	return equal;
}

static bool
patchd_restore(const patchd_target_t *target)
{
	int64_t recovery = nxu_open(target->recovery, NXU_O_READ);
	if (recovery < 0) {
		return false;
	}

	patchd_log("patchd: applying patches to ");
	patchd_log(target->primary);
	patchd_log("\n");

	int64_t primary = nxu_open(target->primary, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);
	if (primary < 0) {
		(void)nxu_close((uint64_t)recovery);
		return false;
	}

	static char buffer[PATCHD_BUFFER_SIZE];
	bool success = true;

	for (;;) {
		int64_t length = nxu_read((uint64_t)recovery, buffer, sizeof(buffer));
		if (length < 0) {
			success = false;
			break;
		}
		if (length == 0) {
			break;
		}

		uint64_t complete = 0ULL;
		while (complete < (uint64_t)length) {
			int64_t written = nxu_write((uint64_t)primary, buffer + complete, (uint64_t)length - complete);
			if (written <= 0) {
				success = false;
				break;
			}
			complete += (uint64_t)written;
		}

		if (!success) {
			break;
		}
	}

	(void)nxu_close((uint64_t)primary);
	(void)nxu_close((uint64_t)recovery);
	if (success) {
		success = nxu_sync() == 0;
	}
	return success;
}

static bool
patchd_repair_requested(void)
{
	int64_t request = nxu_open(PATCHD_REQUEST_PATH, NXU_O_READ);
	if (request < 0) {
		return false;
	}
	(void)nxu_close((uint64_t)request);
	return true;
}

static void
patchd_prepare_requested(void)
{
	const char *request_path = "/disk/var/db/patchd/prepare-update";
	int64_t descriptor = nxu_open(request_path, NXU_O_READ);
	if (descriptor < 0) {
		return;
	}

	char path[256];
	int64_t length = nxu_read(descriptor, path, sizeof(path));
	(void)nxu_close(descriptor);
	/* A terminating newline commits a complete request for the poller. */
	if (length <= 0 || (uint64_t)length >= sizeof(path) || path[length - 1] != '\n') {
		return;
	}
	path[length - 1] = 0;

	const char *prefix = "/Volumes/Recovery/mainOS/";
	uint32_t index = 0;
	while (prefix[index] != 0 && path[index] == prefix[index]) {
		index++;
	}
	bool valid = prefix[index] == 0;
	for (; valid && path[index] != 0; index++) {
		char character = path[index];
		valid = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
				(character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.';
	}
	valid &= (uint64_t)length > nxu_strlen(prefix) + 5 && nxu_streq(path + length - 5, ".nsw");
	bool success = valid && nsw_prepare(path, "/Volumes/Recovery/Prepared");
	if (!valid) {
		patchd_log("patchd: invalid NSW preparation request\n");
	}

	int64_t status = nxu_open("/disk/var/db/patchd/update-status", NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);
	if (status >= 0) {
		const char *message = success ? "prepared\n" : "failed\n";
		(void)nxu_write(status, message, nxu_strlen(message));
		(void)nxu_close(status);
	}
	(void)nxu_unlink(request_path);
	(void)nxu_sync();
}

static bool
patchd_install(const char *build)
{
	if (!nsw_schedule(build)) {
		return false;
	}
	patchd_log("patchd: restarting to install the queued system update\n");
	(void)nxu_system_reset();
	patchd_log("patchd: reset unavailable; installation remains queued for the next boot\n");
	return true;
}

static void
patchd_install_requested(void)
{
	const char *path = "/disk/var/db/patchd/install-update";
	int64_t descriptor = nxu_open(path, NXU_O_READ);
	if (descriptor < 0) {
		return;
	}
	char build[64];
	int64_t length = nxu_read(descriptor, build, sizeof(build));
	(void)nxu_close(descriptor);
	if (length <= 0 || (uint64_t)length >= sizeof(build) || build[length - 1] != '\n') {
		return;
	}
	build[length - 1] = 0;
	/* Consume the request durably before reset so it cannot schedule a second install. */
	if (nxu_unlink(path) != 0 || nxu_sync() != 0) {
		patchd_log("patchd: unable to consume installation request; installation was not queued\n");
		return;
	}
	if (!patchd_install(build)) {
		patchd_log("patchd: installation request rejected\n");
	}
}

static bool
patchd_boot_healthy(void)
{
	for (uint32_t index = 0; index < sizeof(g_targets) / sizeof(g_targets[0]); index++) {
		if (!patchd_images_equal(&g_targets[index])) {
			return false;
		}
	}
	return true;
}

static void
patchd_check_all(bool force)
{
	for (uint32_t index = 0U; index < sizeof(g_targets) / sizeof(g_targets[0]); index++) {
		if (!force && patchd_images_equal(&g_targets[index])) {
			continue;
		}
		if (patchd_restore(&g_targets[index])) {
			patchd_log("patchd: restored system daemon from recovery image\n");
		} else {
			patchd_log("patchd: daemon recovery failed\n");
		}
	}
}

int
main(uint64_t argc, const char *const *argv)
{
	if (argc > 1) {
		if (argc == 3 && nxu_streq(argv[1], "--prepare")) {
			return nsw_prepare(argv[2], NSW_PREPARED_ROOT) ? 0 : 1;
		}
		if (argc == 3 && nxu_streq(argv[1], "--install")) {
			return patchd_install(argv[2]) ? 0 : 1;
		}
		patchd_log("usage: patchd --prepare PACKAGE.nsw | --install BUILD\n");
		return 2;
	}

	patchd_log("patchd: integrity repair service started\n");
	uint64_t started = (uint64_t)nxu_uptime_us();
	bool confirmed = false;
	uint64_t last_check = 0ULL;
	uint64_t last_request_poll = 0ULL;

	for (;;) {
		uint64_t now = (uint64_t)nxu_uptime_us();
		bool forced = false;

		if (now - last_request_poll >= PATCHD_REQUEST_POLL_US) {
			last_request_poll = now;
			forced = patchd_repair_requested();
			patchd_prepare_requested();
			patchd_install_requested();
		}

		if (forced || now - last_check >= PATCHD_INTERVAL_US) {
			patchd_check_all(forced);
			last_check = now;

			if (forced) {
				(void)nxu_unlink(PATCHD_REQUEST_PATH);
				(void)nxu_sync();
			}
		}

		if (!confirmed && now - started >= PATCHD_INTERVAL_US && patchd_boot_healthy()) {
			confirmed = nsw_confirm_boot();
		}

		/* The request file is polled twice a second: sleep between looks. */
		(void)nxu_sleep_us(100000ULL);
	}
}
