#include <frameworks/CoreFoundation.framework/sbin/bootd/job.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <assert.h>
#include <stdio.h>

int64_t nxu_open(const char *path, uint64_t flags)
{
	(void)flags;
	FILE *file = fopen(path, "rb");
	return file ? (int64_t)(uintptr_t)file : -NXU_SYS_E_NOT_FOUND;
}

int64_t nxu_read(uint64_t descriptor, void *buffer, uint64_t length)
{
	return (int64_t)fread(buffer, 1, length, (FILE *)(uintptr_t)descriptor);
}

int64_t nxu_close(uint64_t descriptor)
{
	return fclose((FILE *)(uintptr_t)descriptor);
}

static service_config_t load(const char *root, const char *name)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/frameworks/BootDaemons.framework/Services/com.nxu.%s.plist", root, name);
	service_config_t config;
	assert(service_config_load(path, &config) == SERVICE_CONFIG_OK);
	return config;
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	service_config_t patchd = load(argv[1], "patchd");
	assert(patchd.allow_filesystem_write && patchd.allow_reset);
	service_config_t logd = load(argv[1], "logd");
	assert(logd.allow_filesystem_write && !logd.allow_reset);
	service_config_t desktop = load(argv[1], "desktop");
	bootd_job_t job;
	assert(desktop.keep_alive && desktop.run_at_load && desktop.allow_display);
	assert(nxu_streq(desktop.required_boot_argument, "ui.userland=1"));
	assert(bootd_job_init(&job, &desktop, "", false) && job.state == BOOTD_JOB_DISABLED);
	assert(bootd_job_init(&job, &desktop, "ui.userland=0", false) && job.state == BOOTD_JOB_DISABLED);
	assert(bootd_job_init(&job, &desktop, "ui.userland=1", false) && job.state == BOOTD_JOB_LOADED);
	assert(bootd_job_init(&job, &desktop, "ui.userland=1 -x", true) && job.state == BOOTD_JOB_LOADED);
	assert(bootd_job_init(&job, &desktop, "ui.userland=1 -no-desktop", false) && job.state == BOOTD_JOB_DISABLED);

	service_config_t compositor = load(argv[1], "windowserver");
	assert(bootd_job_init(&job, &compositor, "ui.userland=1 -x", true) && job.state == BOOTD_JOB_LOADED);
	assert(bootd_job_init(&job, &compositor, "-no-windowserver", false) && job.state == BOOTD_JOB_DISABLED);

	service_config_t demo = load(argv[1], "about-sevos");
	assert(bootd_job_init(&job, &demo, "ui.userland=1", false) && job.state == BOOTD_JOB_DISABLED);
	assert(bootd_job_init(&job, &demo, "-about-sevos-demo", false) && job.state == BOOTD_JOB_LOADED);
	service_config_t crash = load(argv[1], "crashtest");
	assert(bootd_job_init(&job, &crash, "ui.userland=1", false) && job.state == BOOTD_JOB_DISABLED);
	assert(bootd_job_init(&job, &crash, "crash.test=1", false) && job.state == BOOTD_JOB_LOADED);
	return 0;
}
