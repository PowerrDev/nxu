#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <kern/console/display_owner.h>
#include <kern/syscall/syscall_defs.h>
#include <kern/process/signal.h>
#include <nxu/runtime.h>
static struct proc procs[3];
static uint32_t restart_pid;

proc_t proc_kernel(void)
{
	return &procs[0];
}

proc_t proc_find_ident(const proc_ident_t *ident)
{
	proc_t proc = proc_find(ident->pid);
	return proc && proc->p_ident.uniqueid == ident->uniqueid && proc->p_ident.idversion == ident->idversion ? proc : 0;
}

void proc_rele(proc_t proc)
{
	(void)proc;
}

signal_send_result_t signal_send(proc_t sender, proc_id_t pid, uint32_t signal)
{
	(void)sender;
	restart_pid = pid;
	assert(signal == NXU_SIGKILL);
	return SIGNAL_SEND_OK;
}

proc_t proc_find(proc_id_t id)
{
	return id > 0 && id < 3 ? &procs[id] : 0;
}

void ui_bridge_host_release(proc_t proc)
{
	(void)proc;
}

bool proc_has_caps(proc_t p, uint32_t caps)
{
	return (p->caps & caps) == caps;
}

static int fail_map, fail_unmap, live;

int64_t nxu_mmap(uint64_t n, uint64_t flags)
{
	(void)flags;
	if (fail_map)
		return -1;
	void *p = malloc(n);
	if (!p)
		return -1;
	live++;
	return (int64_t)(uintptr_t)p;
}

int64_t nxu_munmap(uint64_t a, uint64_t n)
{
	(void)n;
	if (fail_unmap)
		return -1;
	free((void *)(uintptr_t)a);
	live--;
	return 0;
}

static nxu_gui_host_hello_t hello(void)
{
	nxu_gui_host_hello_t r = {0};
	r.header = (nxu_gui_host_header_t){sizeof(r), 1, 0, NXU_GUI_HOST_HELLO, 0, 0, 1};
	r.required_features = r.supported_features = NXU_GUI_HOST_FEATURE_LEASE;
	r.max_record_size = 4096;
	return r;
}

static int call(proc_t p, nxu_gui_host_hello_t *r)
{
	nxu_gui_host_hello_reply_t out;
	int result = display_owner_host(p, r, &out);
	assert(out.status == result);
	assert(out.header.request_id == r->header.request_id);
	return result;
}

int main(void)
{
	for (int i = 1; i < 3; i++) {
		procs[i].p_ident = (proc_ident_t){i, i, 1};
		procs[i].caps = NXU_CAP_DISPLAY;
	}
	proc_t a = &procs[1], b = &procs[2];
	nxu_gui_host_hello_t r = hello();
	a->caps = 0;
	assert(call(a, &r) == -NXU_SYS_E_DENIED);
	a->caps = NXU_CAP_DISPLAY;
	r.header.major = 2;
	assert(call(a, &r) == -NXU_SYS_E_NOT_SUPPORTED);
	assert(!a->p_gui_epoch);
	r = hello();
	r.required_features |= NXU_GUI_HOST_FEATURE_SURFACE;
	r.supported_features = r.required_features;
	assert(call(a, &r) == -NXU_SYS_E_NOT_SUPPORTED);
	assert(!a->p_gui_epoch);
	r = hello();
	assert(call(a, &r) == 0);
	uint64_t old = a->p_gui_epoch;
	assert(old);
	assert(call(a, &r) == -NXU_SYS_E_BUSY);
	r.header = (nxu_gui_host_header_t){32, 1, 0, NXU_GUI_HOST_DISPLAY_ATTACH, 0, old + 1, 2};
	assert(call(a, &r) == -NXU_SYS_E_INVALID_ARGUMENT);
	r.header.session_epoch = old;
	assert(call(a, &r) == 0);
	assert(display_owner_is(1));
	nxu_gui_host_hello_t s = hello();
	assert(call(b, &s) == 0);
	s.header = (nxu_gui_host_header_t){32, 1, 0, NXU_GUI_HOST_DISPLAY_ATTACH, 0, b->p_gui_epoch, 3};
	assert(call(b, &s) == -NXU_SYS_E_BUSY);
	display_owner_release(b);
	assert(display_owner_is(1));
	r.header.operation = NXU_GUI_HOST_RELEASE;
	assert(call(a, &r) == 0);
	assert(!a->p_gui_epoch);
	assert(!display_owner_is(1));
	assert(call(a, &r) == -NXU_SYS_E_INVALID_ARGUMENT);
	r = hello();
	assert(call(a, &r) == 0);
	assert(a->p_gui_epoch != old);
	assert(display_owner_claim(1));
	a->p_ident.uniqueid++;
	assert(!display_owner_is(1));
	a->p_ident.uniqueid--;
	display_owner_release(a);
	a->p_flag = PROC_FLAG_EXITING;
	r = hello();
	assert(call(a, &r) == -NXU_SYS_E_DENIED);
	a->p_flag = 0;
	assert(display_owner_claim(2));
	display_owner_restart();
	assert(restart_pid == 2);
	display_owner_release(b);
	restart_pid = 0;
	display_owner_restart();
	assert(restart_pid == 0);
	assert(display_owner_claim(1));
	display_owner_release(a);
	for (uint64_t align = 1; align <= 65536; align *= 2) {
		void *p = nxu_alloc(127, align);
		assert(p);
		assert(!((uintptr_t)p % align));
		memset(p, 42, 127);
		void *q = nxu_realloc(p, 255, align);
		assert(q);
		for (int i = 0; i < 127; i++)
			assert(((unsigned char *)q)[i] == 42);
		assert(nxu_free(q));
	}
	assert(!nxu_alloc(UINT64_MAX, 16));
	assert(!nxu_alloc(4, 3));
	assert(!nxu_alloc(0, 16));
	assert(nxu_free(0));
	void *p = nxu_alloc(32, 16);
	memset(p, 11, 32);
	fail_map = 1;
	assert(!nxu_realloc(p, 64, 16));
	assert(((char *)p)[0] == 11);
	fail_map = 0;
	fail_unmap = 1;
	assert(!nxu_free(p));
	fail_unmap = 0;
	assert(nxu_free(p));
	assert(!live);
	puts("GUI ownership/epoch policy and Runtime allocator checks passed");
}
