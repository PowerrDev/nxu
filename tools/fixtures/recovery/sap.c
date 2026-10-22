#include <Recovery/RecoveryServices.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/thread.h>
#include <frameworks/Recovery.framework/triageOS/shell.h>

#define TREE "/Volumes/mainOS/System/Library/Frameworks/SAPTree.framework"
#define CHILD TREE "/nested/file.txt"
#define KEEP "/Volumes/mainOS/System/Library/Frameworks/Keep.framework/keep.txt"
static uint32_t pixels[640U * 480U];
static char output[512];
static uint32_t excluded;
static bool probe_done;

static void
require(bool ok, const char *message)
{
	if (ok) {
		return;
	}

	recovery_write_string("RECOVERY SAP FAIL: ");
	recovery_write_string(message);
	recovery_write_string("\n");
	nxu_exit(1);
}

static void
capture(void *context, const char *line)
{
	(void)context;

	uint64_t length = nxu_strlen(line);

	if (length >= sizeof(output)) {
		length = sizeof(output) - 1;
	}

	memcpy(output, line, length);
	output[length] = 0;
	recovery_write_string(line);
	recovery_write_string("\n");
}

static void
input_probe(void *unused)
{
	(void)unused;
	while (!__atomic_load_n(&probe_done, __ATOMIC_ACQUIRE)) {
		nxu_recovery_input_event_t event;
		int64_t result = nxu_recovery_input(&event);

		if (result == -NXU_SYS_E_DENIED) {
			require(nxu_recovery_present(pixels, 640, 0, 0, 1, 1) == -NXU_SYS_E_DENIED, "scanout was not exclusive");
			__atomic_add_fetch(&excluded, 1, __ATOMIC_RELAXED);
		}

		nxu_sleep_us(10000);
	}
}

static bool
exists(const char *path)
{
	nxu_stat_t stat;

	return nxu_stat(path, &stat) == 0;
}

static void
intact(void)
{
	char content[32] = {0};

	require(recovery_fs_read(CHILD, 0, content, sizeof(content)) == 9 && nxu_streq(content, "keep tree"),
			"tree changed before approval");
	require(exists(KEEP), "sibling disappeared");
}

int
main(void)
{
	if (nxu_getpid() != 1) {
		require(nxu_sap_remove_tree(TREE) == -NXU_SYS_E_DENIED, "non-PID-1 recursive operation");
		nxu_exit(0);
	}

	char arguments[512] = {0};

	require(nxu_get_boot_args(arguments, sizeof(arguments)) >= 0, "boot arguments");

	bool unavailable = nxu_arg_present(arguments, "test.sap-unavailable=1");
	int64_t child = nxu_spawn_caps("/Volumes/mainOS/System/Recovery/triageOS", "sap-child", NXU_CAP_FS_WRITE);

	require(child > 0, "spawn child permission probe");

	uint64_t status;
	int64_t waited = -NXU_SYS_E_AGAIN;

	for (uint32_t tries = 0; tries < 100 && waited == -NXU_SYS_E_AGAIN; tries++) {
		waited = nxu_waitpid((uint64_t)child, &status);
		if (waited == -NXU_SYS_E_AGAIN) {
			nxu_sleep_us(10000);
		}
	}

	require(waited == child && status == 0, "child operation refused");
	for (uint32_t i = 0; i < 640U * 480U; i++) {
		pixels[i] = 0xff006040;
	}

	require(nxu_recovery_present(pixels, 640, 0, 0, 640, 480) == 0, "recovery surface");
	require(nxu_sap_remove_tree("/Volumes/mainOS") == -NXU_SYS_E_BUSY, "mount root protection");
	require(nxu_sap_remove_tree(TREE "/../SAPTree.framework") == -NXU_SYS_E_INVALID_ARGUMENT,
			"parent traversal protection");
	require(nxu_sap_delete(TREE) == -NXU_SYS_E_IS_DIRECTORY, "ordinary SAP remains file-only");
	require(nxu_thread_spawn(input_probe, 0, 32768) > 0, "input probe");

	triage_shell_t shell;

	triage_shell_init(&shell, capture, 0);
	triage_shell_execute(&shell, "rm -r " TREE);
	require(nxu_streq(output, "rm: add --force-system to continue"), "force warning");
	intact();
	recovery_write_string("recovery-sap: cancel phase\n");
	triage_shell_execute(&shell, "rm -r --force-system " TREE);
	require(nxu_streq(output, "rm: permission denied"), "cancel result");
	intact();
	recovery_write_string("recovery-sap: wrong-password phase\n");
	triage_shell_execute(&shell, "rm -r --force-system " TREE);
	require(nxu_streq(output, "rm: permission denied"), "wrong password result");
	intact();
	if (unavailable) {
		__atomic_store_n(&probe_done, true, __ATOMIC_RELEASE);
		require(__atomic_load_n(&excluded, __ATOMIC_RELAXED) > 0, "exclusive input untested");
		require(nxu_recovery_present(pixels, 640, 0, 0, 640, 480) == 0, "surface restored");
		require(nxu_sync() == 0, "sync");
		recovery_write_string("RECOVERY SAP UNAVAILABLE PASS\n");
		for (;;) {
			nxu_sleep_us(1000000);
		}
	}

	recovery_write_string("recovery-sap: approved phase\n");
	triage_shell_execute(&shell, "rm -r --force-system " TREE);
	require(!exists(TREE), "approved tree survived");
	require(exists(KEEP), "approved deletion escaped subtree");
	recovery_write_string("recovery-sap: fresh-approval phase\n");
	require(nxu_unlink(KEEP) == -NXU_SYS_E_DENIED, "approval was reusable");
	require(exists(KEEP), "cancelled sibling deletion");
	__atomic_store_n(&probe_done, true, __ATOMIC_RELEASE);
	require(__atomic_load_n(&excluded, __ATOMIC_RELAXED) > 0, "exclusive input untested");
	require(nxu_recovery_present(pixels, 640, 0, 0, 640, 480) == 0, "surface restored");
	require(nxu_sync() == 0, "sync");
	recovery_write_string("RECOVERY SAP PASS\n");
	for (;;) {
		nxu_sleep_us(1000000);
	}
}
