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
	{ "/disk/System/Library/CoreServices/bootd", "/disk/System/Library/CoreServices/bootd.recovery" },
	{ "/disk/System/Library/CoreServices/logd", "/disk/System/Library/CoreServices/logd.recovery" },
	{ "/disk/System/Library/CoreServices/patchd", "/disk/System/Library/CoreServices/patchd.recovery" }
};

static void
patchd_log(const char *message)
{
	(void)nxu_write(1U, message, nxu_strlen(message));
}

static bool
patchd_images_equal(const patchd_target_t *target)
{
	int64_t primary = nxu_open(target->primary, NXU_O_READ);
	if (primary < 0) return false;
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

		if (primary_read == 0) break;

		for (int64_t index = 0; index < primary_read; index++) {
			if (primary_buffer[index] == recovery_buffer[index]) continue;
			equal = false;
			break;
		}

		if (!equal) break;

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
	if (recovery < 0) return false;

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
		if (length == 0) break;

		uint64_t complete = 0ULL;
		while (complete < (uint64_t)length) {
			int64_t written = nxu_write((uint64_t)primary, buffer + complete, (uint64_t)length - complete);
			if (written <= 0) {
				success = false;
				break;
			}
			complete += (uint64_t)written;
		}

		if (!success) break;
	}

	(void)nxu_close((uint64_t)primary);
	(void)nxu_close((uint64_t)recovery);
	if (success) success = nxu_sync() == 0;
	return success;
}

static bool
patchd_repair_requested(void)
{
	int64_t request = nxu_open(PATCHD_REQUEST_PATH, NXU_O_READ);
	if (request < 0) return false;
	(void)nxu_close((uint64_t)request);
	return true;
}

static void
patchd_check_all(bool force)
{
	for (uint32_t index = 0U; index < sizeof(g_targets) / sizeof(g_targets[0]); index++) {
		if (!force && patchd_images_equal(&g_targets[index])) continue;
		if (patchd_restore(&g_targets[index])) patchd_log("patchd: restored system daemon from recovery image\n");
		else patchd_log("patchd: daemon recovery failed\n");
	}
}

int
main(void)
{
	patchd_log("patchd: integrity repair service started\n");
	uint64_t last_check = 0ULL;
	uint64_t last_request_poll = 0ULL;

	for (;;) {
		uint64_t now = (uint64_t)nxu_uptime_us();
		bool forced = false;

		if (now - last_request_poll >= PATCHD_REQUEST_POLL_US) {
			last_request_poll = now;
			forced = patchd_repair_requested();
		}

		if (forced || now - last_check >= PATCHD_INTERVAL_US) {
			patchd_check_all(forced);
			last_check = now;

			if (forced) {
				(void)nxu_unlink(PATCHD_REQUEST_PATH);
				(void)nxu_sync();
			}
		}

		/* The request file is polled twice a second: sleep between looks. */
		(void)nxu_sleep_us(100000ULL);
	}
}
