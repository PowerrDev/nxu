#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdint.h>

#define LOGD_BUFFER_SIZE 256U

static void
logd_stderr(const char *message)
{
	(void)nxu_write(2U, message, nxu_strlen(message));
}

int
main(void)
{
	int64_t log = nxu_open("/disk/var/log/system.log", NXU_O_WRITE | NXU_O_CREATE | NXU_O_APPEND);
	if (log < 0) {
		logd_stderr("logd: unable to open /disk/var/log/system.log\n");
		return 1;
	}

	uint64_t cursor = 0ULL;
	char buffer[LOGD_BUFFER_SIZE];

	for (;;) {
		int64_t length = nxu_klog_read(&cursor, buffer, sizeof(buffer));
		if (length < 0) return 2;

		if (length == 0) {
			/* Nothing new: look again in a moment rather than spin. */
			(void)nxu_sleep_us(50000ULL);
			continue;
		}

		uint64_t complete = 0ULL;
		while (complete < (uint64_t)length) {
			int64_t written = nxu_write((uint64_t)log, buffer + complete, (uint64_t)length - complete);
			if (written <= 0) return 3;
			complete += (uint64_t)written;
		}
	}
}
