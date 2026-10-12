#include <Recovery/RecoveryServices.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <frameworks/Recovery.framework/triageOS/shell.h>

static char output[1024];

static void
capture(void *context, const char *text)
{
	(void)context;

	uint64_t length = nxu_strlen(text);

	if (length >= sizeof(output)) {
		length = sizeof(output) - 1;
	}

	memcpy(output, text, length);
	output[length] = 0;
	recovery_write_string(text);
	recovery_write_string("\n");
}

static void
require(bool condition, const char *message)
{
	if (condition) {
		return;
	}

	recovery_write_string("RECOVERY STORAGE FAIL: ");
	recovery_write_string(message);
	recovery_write_string("\n");
	nxu_exit(1);
}

int
main(void)
{
	triage_shell_t shell;

	triage_shell_init(&shell, capture, 0);
	require(nxu_getpid() == 1, "recovery is PID 1");
	require(recovery_fs_mkdir("/disk/remove") == 0, "mkdir parent");
	require(recovery_fs_mkdir("/disk/remove/nested") == 0, "mkdir nested");
	require(recovery_fs_write("/disk/remove/nested/file", "test", 4) == 4, "create child");
	require(nxu_rmdir("/disk/remove") == -NXU_SYS_E_NOT_EMPTY, "reject nonempty directory");
	require(nxu_rmdir("/disk") == -NXU_SYS_E_BUSY, "reject mount root");
	triage_shell_execute(&shell, "rm /disk/remove");
	require(nxu_streq(output, "rm: is a directory (use rm -r)"), "directory diagnostic");
	triage_shell_execute(&shell, "rm -r /disk/remove");

	nxu_stat_t stat;

	require(nxu_stat("/disk/remove", &stat) == -NXU_SYS_E_NOT_FOUND, "recursive removal");
	require(recovery_fs_write("/disk/file", "test", 4) == 4, "create regular file");
	triage_shell_execute(&shell, "rm /disk/file");
	require(nxu_stat("/disk/file", &stat) == -NXU_SYS_E_NOT_FOUND, "regular removal");
	triage_shell_execute(&shell, "partition-delete disk0 0 --force");
	require(nxu_streq(output, "partition-delete: busy"), "mounted device refused");
	triage_shell_execute(&shell, "partition-delete disk1 0");
	require(nxu_streq(output, "partition-delete: destructive; add --force to continue"), "force required");
	triage_shell_execute(&shell, "partition-delete disk1 0 --force");
	require(nxu_streq(output, "partition-delete: partition deleted"), "MBR delete");

	recovery_block_layout_info_t layout;

	require(recovery_block_layout_info(1, &layout) == 0 && layout.partition_count == 0, "partition removed");
	triage_shell_execute(&shell, "partition-delete disk2 1 --force");
	require(nxu_streq(output, "partition-delete: partition deleted"), "GPT second delete");
	require(recovery_block_layout_info(2, &layout) == 0 && layout.partition_count == 1, "GPT survivor");

	recovery_block_partition_info_t survivor;

	require(recovery_block_partition_info(2, 0, &survivor) == 0 && survivor.start_sector == 2048,
			"GPT first preserved");
	triage_shell_execute(&shell, "partition-delete disk2 0 --force");
	require(nxu_streq(output, "partition-delete: partition deleted"), "GPT first delete");
	require(recovery_block_layout_info(2, &layout) == 0 && layout.partition_count == 0, "GPT emptied");
	triage_shell_execute(&shell, "partition-delete disk3 0 --force");
	require(nxu_streq(output, "partition-delete: read-only filesystem or device"), "read-only diagnostic");
	require(recovery_fs_sync() == 0, "sync");
	recovery_write_string("RECOVERY STORAGE PASS\n");
	for (;;) {
		nxu_sleep_us(1000000);
	}
}
