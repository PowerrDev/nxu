/* Exercise the production manager with deterministic registry/job mocks. */
#include <frameworks/CoreFoundation.framework/sbin/bootd/manager.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <assert.h>
#include <stdio.h>

static uint32_t registry_polls, job_polls;
static bool transition;
static nxu_ipc_peer_t bound;

int64_t nxu_uptime_us(void)
{
	return 1000000;
}

void bootd_registry_bind_compositor(bootd_registry_t *registry, const nxu_ipc_peer_t *identity)
{
	(void)registry;
	bound = *identity;
}

void bootd_registry_poll(bootd_registry_t *registry)
{
	(void)registry;
	registry_polls++;
}

void bootd_job_poll(bootd_job_t *job, uint64_t now_us)
{
	assert(now_us == 1000000);
	job_polls++;
	if (transition && nxu_streq(job->config.label, "com.nxu.windowserver")) {
		job->state = BOOTD_JOB_EXITED;
		transition = false;
	}
}

int main(void)
{
	bootd_manager_t manager = {0};
	manager.job_count = BOOTD_MAX_JOBS;
	for (uint32_t i = 0; i < manager.job_count; i++)
		manager.jobs[i].state = BOOTD_JOB_DISABLED;

	bootd_job_t *compositor = &manager.jobs[0];
	memcpy(compositor->config.label, "com.nxu.windowserver", 20);
	compositor->state = BOOTD_JOB_RUNNING;
	compositor->identity = (nxu_ipc_peer_t){.pid = 7, .uniqueid = 70};
	for (uint32_t i = 0; i < 1000; i++)
		bootd_manager_poll(&manager);

	assert(registry_polls == 2000 && job_polls == 32000 && bound.uniqueid == 70);
	printf("PASS idle manager: 32 jobs, 1000 cycles, 2000 registry polls (previously 33000)\n");

	registry_polls = 0;
	transition = true;
	bootd_manager_poll(&manager);
	assert(registry_polls == 3 && !bound.uniqueid);

	registry_polls = 0;
	manager.jobs[1].state = BOOTD_JOB_LOADED;
	manager.jobs[1].activation_requested = true;
	bootd_manager_poll(&manager);
	assert(registry_polls == 3);

	registry_polls = 0;
	manager.jobs[1].state = BOOTD_JOB_THROTTLED;
	manager.jobs[1].restart_after_us = 2000000;
	bootd_manager_poll(&manager);
	assert(registry_polls == 2);
	puts("PASS state transitions revoke compositor and due spawns service registry promptly");
	return 0;
}
