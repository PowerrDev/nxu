/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <frameworks/CoreFoundation.framework/sbin/bootd/manager.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

static bootd_manager_t g_manager;
static char g_output[16384];

static bool
contains(const char *text, const char *needle)
{
	uint64_t length = nxu_strlen(needle);

	for (; *text; text++) {
		uint64_t index = 0;

		while (index < length && text[index] != 0 && text[index] == needle[index]) {
			index++;
		}

		if (index == length) {
			return true;
		}
	}

	return false;
}

static void
pump(uint64_t duration)
{
	uint64_t deadline = nxu_uptime_us() + duration;

	do {
		bootd_manager_poll(&g_manager);
		(void)nxu_sleep_us(1000);
	} while ((uint64_t)nxu_uptime_us() < deadline);
}

static void
terminal_ui_test(void)
{
	for (uint32_t index = 0; index < g_manager.job_count; index++) {
		bootd_job_t *job = &g_manager.jobs[index];
		service_config_t config = job->config;

		if (nxu_streq(config.label, "com.nxu.windowserver")) {
			(void)bootd_job_init(job, &config, "", false);
			bootd_job_activate(job);
		}
	}

	bootd_manager_start(&g_manager);
	pump(3000000);
	service_config_t config = {0};
	memcpy(config.label, "com.nxu.desktop", sizeof("com.nxu.desktop"));
	memcpy(config.program, "/Volumes/mainOS/System/Library/CoreServices/desktop_service",
		   sizeof("/Volumes/mainOS/System/Library/CoreServices/desktop_service"));
	config.keep_alive = true;
	config.allow_display = true;
	bootd_job_t *desktop = &g_manager.jobs[g_manager.job_count++];
	(void)bootd_job_init(desktop, &config, "", false);
	bootd_job_activate(desktop);
	pump(5000000);
	int64_t child = nxu_spawn("/Volumes/mainOS/System/Library/CoreServices/terminal", "Terminal");
	const char started[] = "terminal-ui: launched\n";
	(void)nxu_write(1, started, sizeof(started) - 1);

	while (child > 0) {
		pump(1000);
		uint64_t status;
		int64_t result = nxu_waitpid(child, &status);

		if (result == -NXU_SYS_E_AGAIN) {
			continue;
		}

		const char *message = result > 0 && status == 0 ? "terminal-ui: clean exit passed\n" : "terminal-ui: FAILED\n";
		(void)nxu_write(1, message, nxu_strlen(message));
		break;
	}
}

int
main(void)
{
	if (!bootd_manager_init(&g_manager)) {
		return 1;
	}

	int64_t listener = nxu_socket_listen("com.nxu.test.shell", 1);
	int64_t child = listener < 0 ? -1 : nxu_fork();

	if (child == 0) {
		(void)nxu_close(listener);
		int64_t stream = nxu_socket_connect("com.nxu.test.shell");

		if (stream < 0 || nxu_dup2(stream, 0) != 0 || nxu_dup2(stream, 1) != 1 || nxu_dup2(stream, 2) != 2) {
			return 2;
		}

		(void)nxu_close(stream);
		const char *arguments[] = {"nzh", 0};
		(void)nxu_exec("/Volumes/mainOS/System/Library/CoreServices/nzh", arguments);
		return 3;
	}

	if (child < 0) {
		return 4;
	}

	int64_t stream = nxu_socket_accept(listener);
	(void)nxu_close(listener);
	const char commands[] = "echo 'quoted shell arguments'\n"
							"ifconfig\n"
							"ping -c 1 127.0.0.1\n"
							"ping -6 -c 1 ::1\n"
							"ping -c 1 10.0.2.2\n"
							"ping -6 -c 1 fec0::2\n"
							"ipctest\n"
							"sockettest\n"
							"terminaltest\n"
							"echo SESSION-COMPLETE\nexit\n";

	if (stream < 0 || nxu_write(stream, commands, sizeof(commands) - 1) != sizeof(commands) - 1) {
		return 5;
	}

	uint32_t offset = 0;
	uint64_t deadline = nxu_uptime_us() + 60000000;
	bool eof = false;

	while ((uint64_t)nxu_uptime_us() < deadline && offset + 1 < sizeof(g_output)) {
		bootd_manager_poll(&g_manager);
		int64_t count = nxu_read_poll(stream, g_output + offset, sizeof(g_output) - offset - 1);

		if (count == 0) {
			eof = true;
			break;
		}

		if (count > 0) {
			(void)nxu_write(1, g_output + offset, count);
			offset += count;
		} else if (count != -NXU_SYS_E_AGAIN) {
			break;
		}

		(void)nxu_sleep_us(1000);
	}

	g_output[offset] = 0;
	(void)nxu_close(stream);
	uint64_t status = 0;

	if (!eof) {
		(void)nxu_kill(child, NXU_SIGKILL);
	}

	bool passed = eof && nxu_wait(child, &status) >= 0 && status == 0 && contains(g_output, "quoted shell arguments") &&
				  contains(g_output, "SESSION-COMPLETE") && contains(g_output, "ipctest_b: received \"pong\"") &&
				  contains(g_output, "terminaltest: fork/exec, dup2, stdout/stderr, backpressure and EOF passed") &&
				  !contains(g_output, "command exited") && !contains(g_output, "timed out") &&
				  !contains(g_output, "FAILED");
	const char *message =
		passed ? "shell-session: networking and executable tests passed\n" : "shell-session: FAILED\n";
	(void)nxu_write(1, message, nxu_strlen(message));

	if (passed && nxu_arg_present(g_manager.boot_args, "terminal.test=1")) {
		terminal_ui_test();
	}

	for (;;) {
		(void)nxu_sleep_us(1000000);
	}
}
